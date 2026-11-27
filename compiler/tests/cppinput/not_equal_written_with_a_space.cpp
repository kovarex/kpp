// cppinput: an operator!= written `operator !=` (Unreal's FIoChunkId: `inline bool operator !=(const FIoChunkId& Rhs) const
// { return !(*this == Rhs); }`) is dropped as `operator!=` is, a != b being !(a == b); the converter never read a file whose
// operator!= had a space before its `!=`, and kept it, an error in kpp
struct Id
{
  int Value = 0;
  inline bool operator ==(const Id& Rhs) const { return Value == Rhs.Value; }
  inline bool operator !=(const Id& Rhs) const { return !(*this == Rhs); }
};

int main()
{
  Id a{1}, b{1}, c{2};
  return a == b && a != c && !(a != b) ? 0 : 1;
}
