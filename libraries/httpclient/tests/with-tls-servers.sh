#!/bin/sh
# Runs a command with two HTTPS servers of its own on the loopback, for httpclient's test
# tlsWhenAskedFor (HttpClientTests.kpp): no site outside the machine decides whether it passes.
# Each server is OpenSSL's s_server with a certificate made here, for the name localhost:
#   - the first one's certificate is the file SSL_CERT_FILE names for the command, so OpenSSL's
#     default verify paths (which httpclient asks for) trust it;
#   - the second one's is in no such file: nothing vouches for it.
# Both answer GET /page with the file `page` as it is: a whole HTTP response, 48,000 bytes of
# body by Content-Length (three TLS records). The command gets HTTPCLIENT_TEST_TLS="PORT PORT"
# (the trusted server's, then the other's) and SSL_CERT_FILE; its exit code is this script's.
# Anything that does not start is an error here, never a run without the servers.
#   sh with-tls-servers.sh COMMAND [ARGUMENT...]
#   HTTPCLIENT_TEST_TLS_PORTS="18443 18444" names other ports (these two are the default).

# The reason last, after what the failed step wrote ($2, a file): CI's summary is a log's last line.
failed()
{
  [ -z "$2" ] || cat "$2"
  echo "with-tls-servers.sh: FAILED: $1"
  exit 1
}

[ $# -gt 0 ] || failed "no command to run"
ports=${HTTPCLIENT_TEST_TLS_PORTS:-18443 18444}
trusted=${ports% *} other=${ports#* }
command -v openssl > /dev/null 2>&1 || failed "no openssl command"
dir=$(mktemp -d /tmp/kpp-httpclient-tls.XXXXXX) || failed "no directory of its own in /tmp"
pids=""
stop()
{
  [ -z "$pids" ] || kill $pids 2> /dev/null
  rm -rf "$dir"
}
trap stop EXIT
trap 'exit 1' INT TERM HUP

for name in trusted other; do
  openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" \
    -keyout "$dir/$name.key" -out "$dir/$name.pem" > "$dir/$name.req.log" 2>&1 ||
    failed "openssl made no certificate ($name)" "$dir/$name.req.log"
done

i=0
while [ $i -lt 1000 ]; do
  echo "kpp httpclient over TLS, one line of a thousand"
  i=$((i + 1))
done > "$dir/body"
{
  printf 'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' "$(wc -c < "$dir/body" | tr -d ' ')"
  cat "$dir/body"
} > "$dir/page"

# A server with certificate $1 on port $2, and the wait until it answers a TLS handshake. (-HTTP:
# a file's bytes are the response. On the loopback's IPv4 address only, so that the name 127.0.0.1
# reaches a server too: one whose certificate does not carry it.) A server that has ended (its
# port taken) or does not answer in 50 tries is an error, with what it wrote.
serve()
{
  (cd "$dir" && exec openssl s_server -accept "127.0.0.1:$2" -cert "$1.pem" -key "$1.key" -HTTP -quiet) > "$dir/$1.log" 2>&1 &
  pid=$!
  pids="$pids $pid"
  tries=0
  while :; do
    kill -0 $pid 2> /dev/null || failed "the server on 127.0.0.1:$2 has ended ($1)" "$dir/$1.log"
    if timeout 5 openssl s_client -connect "127.0.0.1:$2" < /dev/null > /dev/null 2>&1; then
      return
    fi
    tries=$((tries + 1))
    [ $tries -lt 50 ] || failed "the server on 127.0.0.1:$2 answers no TLS handshake ($1)" "$dir/$1.log"
    sleep 0.2
  done
}
serve trusted "$trusted"
serve other "$other"

SSL_CERT_FILE="$dir/trusted.pem" HTTPCLIENT_TEST_TLS="$trusted $other" "$@"
code=$?
exit $code
