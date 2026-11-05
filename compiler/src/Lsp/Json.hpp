#pragma once
// Part of kppc --lsp: lsp.cpp includes it where it was.

struct Json
{
  enum Kind : uint8_t { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : kind(Bool), boolean(b) {}
  template<typename T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
  Json(T n) : kind(Number), number(double(n)) {}
  Json(const char* s) : kind(String), text(s) {}
  Json(std::string s) : kind(String), text(std::move(s)) {}
  static Json array()
  {
    Json j;
    j.kind = Array;
    return j;
  }
  static Json object()
  {
    Json j;
    j.kind = Object;
    return j;
  }
  const Json& operator[](std::string_view key) const
  {
    static const Json none;
    for (const auto& [name, value] : members)
      if (name == key)
        return value;
    return none;
  }
  bool has(std::string_view key) const { return (*this)[key].kind != Null; }
  Json& set(std::string key, Json value)
  {
    kind = Object;
    members.emplace_back(std::move(key), std::move(value));
    return *this;
  }
  Json& push(Json value)
  {
    kind = Array;
    items.push_back(std::move(value));
    return *this;
  }
  int64_t integer() const { return int64_t(number); }

  Kind kind = Null;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;
};
