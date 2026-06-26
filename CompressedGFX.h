#include <Adafruit_GFX.h>

struct CompLink8  {
  uint16_t next;
  uint8_t color;
};

class CompLine8  {
  int w, size;
  CompLink8 *list;
  CompLine8(int w_, size_t size_)  {
    w = w_;
    size = size_;
    list = (CompLink8*)calloc(size, sizeof(CompLink8));
  }
  ~CompLine8() {
    free(list);
  }
  void fill(uint8_t color)  {
    list[0].next = w;
    list[0].color = color;
  }
  void set(uint16_t x, uint8_t color) {
    
  }
};

class GFXCompCanvas8 : public Adafruit_GFX::Adafruit_GFX {
public:
  GFXCompCanvas8(uint16_t w, uint16_t h, int line_size)  {
    WIDTH = w;
    HEIGHT = h;
  }
  ~GFXCompCanvas8(void);
  void drawPixel(int16_t x, int16_t y, uint16_t color);
  void fillScreen(uint16_t color);
  void byteSwap(void);
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  uint16_t getPixel(int16_t x, int16_t y) const;
  /**********************************************************************/
  /*!
    @brief    Get a pointer to the internal buffer memory
    @returns  A pointer to the allocated buffer
  */
  /**********************************************************************/

protected:
  uint16_t getRawPixel(int16_t x, int16_t y) const;
  void drawFastRawVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawFastRawHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  bool buffer_owned; ///< If true, destructor will free buffer, else it will do
                     ///< nothing
};