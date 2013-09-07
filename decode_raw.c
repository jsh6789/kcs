#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static unsigned KCS_FRAMERATE = 44100;
static unsigned KCS_ONES_FREQ = 2400;
static unsigned KCS_ZERO_FREQ = 1200;
static unsigned KCS_ONES_CYCLES = 8;
static unsigned KCS_ZERO_CYCLES = 4;
static double KCS_SQUELCH = 0.25;

#define BLOCKSIZE 19408

int max(int x,int y){
 return (x > y)?x:y;
}

int min(int x,int y){
 return (x < y)?x:y;
}

char *kcs_decode_block(
 int16_t *data,
 unsigned data_length,
 unsigned *offset, /* Offset used for next function call */
 unsigned *length
){
 /* Decodes a sample block and produces decoded characters as output. */
 
 int ones_length = round((double)KCS_FRAMERATE/KCS_ONES_FREQ);
 int zero_length = round((double)KCS_FRAMERATE/KCS_ZERO_FREQ);
 int ones_tolerance = 
  (max(ones_length,zero_length) - min(ones_length,zero_length))/4;
 int zero_tolerance = 
  (max(ones_length,zero_length) - min(ones_length,zero_length))/4;
 int16_t sql_pulse = fmin(1.0,fmax(0.0,KCS_SQUELCH)) * INT16_MAX;
 int distance;
 int ones_distance,zero_distance;

 unsigned char *cyclefreq = NULL;
 unsigned cyclefreq_length = 0;
 unsigned short *cyclefreq_incs = NULL;
 char *text = NULL;
 unsigned text_length = 0;
 unsigned last_text = data_length;
 unsigned data_pos1,data_pos2,data_pos3;
 unsigned pos1,pos2,pos3,x;
 char decoded_byte;
 
 /* === CYCLEFREQ DECODING === */
 
 /* Find the first sample that has a higher value than sql_pulse. */
 for(pos1 = 0;(pos1 < data_length)?(data[pos1] <= sql_pulse):0;pos1++);
 
 /* Go to the first zero cross */
 for(;(pos1 < data_length)?(data[pos1] >= 0):0;pos1++);
 
 do{
  
  /* Seek to the next cycle of the (possible) wave */
  for(pos2 = pos1 + 1;(pos2 < data_length)?(data[pos2] < 0):0;pos2++);
  for(pos2++;(pos2 < data_length)?(data[pos2] >= 0):0;pos2++);
  
  /* Skip it if its amplitude is not high enough */
  for(pos3 = pos1;pos3 < pos2 && data[pos3] < sql_pulse;pos3++);
  if(pos3 == pos2){
   pos1 = pos2;
   continue;
  }
  
  if(pos2 < data_length){
   
   /* Append the appropriate value to cyclefreq. */
   distance = pos2 - pos1;
   ones_distance =
    (distance - ones_length < 0)?ones_length - distance:distance - ones_length;
   zero_distance =
    (distance - zero_length < 0)?zero_length - distance:distance - zero_length;
   if(
    distance <= max(ones_length,zero_length) +
    (ones_distance < zero_distance)?ones_tolerance:zero_tolerance &&
    distance >= min(ones_length,zero_length) -
    (ones_distance < zero_distance)?ones_tolerance:zero_tolerance
   ){
    cyclefreq = realloc(cyclefreq,++cyclefreq_length * sizeof(*cyclefreq));
    cyclefreq_incs =
     realloc(cyclefreq_incs,cyclefreq_length * sizeof(*cyclefreq_incs));
    cyclefreq_incs[cyclefreq_length - 1] = pos2 - pos1;
    if(ones_distance < zero_distance)
     cyclefreq[cyclefreq_length - 1] = 1;
    if(zero_distance < ones_distance)
     cyclefreq[cyclefreq_length - 1] = 0;
   }
  }
  pos1 = pos2;
  
 }while(pos1 < data_length);
 
 /* ===TEXT DECODING === */
 
 pos1 = data_pos1 = 0;
 do{
  
  /* Seek to the beginning of the start bit */
  for(
   ;
   (pos1 < cyclefreq_length)?(cyclefreq[pos1] == 1):0;
   data_pos1 += cyclefreq_incs[pos1],pos1++
  );
  
  
  
  /* Verify the start bit */
  for(
   data_pos2 = data_pos1,pos2 = pos1;
   (pos2 < cyclefreq_length)?
   (pos1 + KCS_ZERO_CYCLES > pos2 && cyclefreq[pos2] == 0):0;
   data_pos2 += cyclefreq_incs[pos2],pos2++
  );
  if(pos1 + KCS_ZERO_CYCLES != pos2)
   goto skip_bad;
  
  /* Read the data bits */
  for(decoded_byte = 0x0,x = 0x1;x <= 0x80;x <<= 1){
   for(
    data_pos3 = data_pos2,pos3 = pos2;
    (pos3 < cyclefreq_length)?
    (pos2 + KCS_ONES_CYCLES > pos3 && cyclefreq[pos3] == 1):0;
    data_pos3 += cyclefreq_incs[pos3],pos3++
   );
   if(pos2 + KCS_ONES_CYCLES == pos3){
    data_pos2 = data_pos3;
    pos2 = pos3;
    decoded_byte |= x;
    continue;
   }
   for(
    data_pos3 = data_pos2,pos3 = pos2;
    (pos3 < cyclefreq_length)?
    (pos2 + KCS_ZERO_CYCLES > pos3 && cyclefreq[pos3] == 0):0;
    data_pos3 += cyclefreq_incs[pos3],pos3++
   );
   if(pos2 + KCS_ZERO_CYCLES == pos3){
    data_pos2 = data_pos3;
    pos2 = pos3;
   }
  }
   
  /* Verify stop bits */
  for(
   data_pos3 = data_pos2,pos3 = pos2;
   (pos3 < cyclefreq_length)?
   (pos2 + KCS_ONES_CYCLES * 2 > pos3 && cyclefreq[pos3] == 1):0;
   data_pos3 += cyclefreq_incs[pos3],pos3++
  );
  if(pos2 + KCS_ONES_CYCLES * 2 != pos3)
   goto skip_bad;
  
  /* Append the value to text */
  text = realloc(text,++text_length * sizeof(*text));
  text[text_length - 1] = decoded_byte;
  
  data_pos1 = data_pos3;
  last_text = data_pos1;
  pos1 = pos3;
  
  continue;
  skip_bad:
  for(
   data_pos2 = data_pos1,pos2 = pos1;
   (pos1 < cyclefreq_length)?
   (cyclefreq[pos1] == 0 && pos2 - pos1 < KCS_ZERO_CYCLES):0;
   data_pos2 += cyclefreq_incs[pos2],pos2++
  );
  data_pos1 = data_pos2;
  pos1 = pos2;
  
 }while(pos1 < cyclefreq_length);
 
 
 free(cyclefreq);
 free(cyclefreq_incs);
 *offset = last_text;
 
 if(text_length == 0){
  text = malloc(1);
  *length = 0;
  return text;
 }
 
 *length = text_length;
 return text;
}

int main(int argc,char *argv[]){
 int16_t *data = NULL;
 unsigned data_length;
 unsigned offset = BLOCKSIZE;
 char *text = NULL;
 unsigned text_length;
 
 data = malloc(BLOCKSIZE * sizeof(*data));
 while(!feof(stdin) && !ferror(stdin)){
  data_length = fread(data + BLOCKSIZE - offset,sizeof(*data),offset,stdin) + BLOCKSIZE - offset;
  text = kcs_decode_block(data,data_length,&offset,&text_length);
  fwrite(text,sizeof(*text),text_length,stdout);
  free(text);
  
  memmove(data,data + offset,(BLOCKSIZE - offset) * sizeof(*data));
 }
 
 return 0;
}
