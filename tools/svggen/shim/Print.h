#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
class Print {
public:
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *b, size_t n){ size_t c=0; while(n--) c+=write(*b++); return c; }
  size_t write(const char *s){ return s?write((const uint8_t*)s, strlen(s)):0; }
  size_t print(const char *s){ return write(s); }
};
