#include <pgmspace.h>

extern const unsigned short bigben_background[] PROGMEM ;
extern const unsigned short mondaine_background[] PROGMEM ;
extern const unsigned short bigben_hour_hand[] PROGMEM ;
extern const unsigned short bigben_minute_hand[] PROGMEM ;
extern const unsigned short bigben_second_hand[] PROGMEM ;
extern const unsigned short mondaine_hour_hand[] PROGMEM ;
extern const unsigned short mondaine_minute_hand[] PROGMEM ;
extern const unsigned short mondaine_second_hand[] PROGMEM ;

struct CLOCK_FACE
{
  char HOUR_HAND_WIDTH;
  char HOUR_HAND_HEIGHT;
  char HOUR_HAND_CENTER_X;
  char HOUR_HAND_CENTER_Y;

  char MINUTE_HAND_WIDTH;
  char MINUTE_HAND_HEIGHT;
  char MINUTE_HAND_CENTER_X;
  char MINUTE_HAND_CENTER_Y;

  char SECOND_HAND_WIDTH;
  char SECOND_HAND_HEIGHT;
  char SECOND_HAND_CENTER_X;
  char SECOND_HAND_CENTER_Y;

  const unsigned short* BACKGROUND;
  const unsigned short* HOUR_HAND;
  const unsigned short* MINUTE_HAND;
  const unsigned short* SECOND_HAND;
};

const CLOCK_FACE CLOCK_FACES[] PROGMEM =
{
  { 22, 84, 11, 58, 19, 150, 9, 119, 11, 119, 5, 113, bigben_background, bigben_hour_hand, bigben_minute_hand, bigben_second_hand }, 
  { 22, 103, 11, 79, 13, 128, 6, 109, 25, 125, 13, 85, mondaine_background, mondaine_hour_hand, mondaine_minute_hand, mondaine_second_hand },
};
