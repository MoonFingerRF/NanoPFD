#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>
#include "Print.h"
typedef uint8_t byte; typedef bool boolean;
class __FlashStringHelper {};
class String {
  std::string s_;
public:
  String(const char *c=""){ if(c) s_=c; }
  unsigned length() const { return (unsigned)s_.size(); }
  char operator[](int i) const { return s_[i]; }
  const char *c_str() const { return s_.c_str(); }
};
#define PROGMEM
#define F(x) (x)
#define pgm_read_byte(a)  (*(const uint8_t  *)(a))
#define pgm_read_word(a)  (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#define memcpy_P memcpy
#ifndef PI
#define PI 3.14159265358979323846
#endif
#ifndef _swap_int16_t
#define _swap_int16_t(a,b){int16_t t=a;a=b;b=t;}
#endif
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif
#ifndef abs
#define abs(x) ((x)>0?(x):-(x))
#endif
#ifndef constrain
#define constrain(x,lo,hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
#endif
static inline float radians(float d){ return d*(float)PI/180.0f; }
static inline float degrees(float r){ return r*180.0f/(float)PI; }
static inline unsigned long millis(){ return 0; }
static inline unsigned long micros(){ return 0; }
static inline void yield(){}
#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
static inline int  analogRead(int){ return 0; }
static inline void analogWrite(int,int){}
static inline int  digitalRead(int){ return 0; }
static inline void digitalWrite(int,int){}
static inline void pinMode(int,int){}
static inline void delay(unsigned long){}
static inline void delayMicroseconds(unsigned){}
static inline long map(long x,long a,long b,long c,long d){ return (b==a)?c:(x-a)*(d-c)/(b-a)+c; }
