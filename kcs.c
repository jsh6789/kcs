/* KiloCycleS KCS Modem
   WIP Rev 8
   Author: JSH
   
   ABOUT
    KiloCycleS encodes and decodes binary data to/from Kansas City Standard
    audio. It is most typically found on vintage computer cassette tapes, but
    it can be used in any case where data needs to be transferred via sound,
    including from speaker to microphone. This version of KiloCycleS includes
    FLAC support for both encoding and decoding; future versions may include
    libsndfile support which would allow use of multiple (lossy) codecs.
    
    For more info and history, see:
    http://en.wikipedia.org/wiki/Kansas_City_standard
    
   TODO
    - fix decoding issue
    - FLAC decoding function
    - FLAC error checking
    - nonstandard options/presets
    - cleanup
    
   CHANGES
    - WIP Rev 1; Initial WIP
    - WIP Rev 6; Implemented decoding routines
    - WIP Rev 7; Null pulse
    - WIP Rev 8; Revise decoding
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <pulse/simple.h>
#include <pulse/error.h>

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>
#include <vorbis/vorbisenc.h>

#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#define ENC_BLOCKSIZE 128

int max(int x,int y){
 return (x > y)?x:y;
}

int min(int x,int y){
 return (x < y)?x:y;
}

typedef int16_t * (*wave_function)(unsigned,unsigned,unsigned *);
static unsigned KCS_FRAMERATE = 44100;
static unsigned KCS_ONES_FREQ = 2400;
static unsigned KCS_ZERO_FREQ = 1200;
static unsigned KCS_ONES_CYCLES = 2;
static unsigned KCS_ZERO_CYCLES = 1;
static unsigned KCS_NULL_CYCLES = 800;
static double KCS_AMPLITUDE = 0.8;
static double KCS_SQUELCH = 0.25;
static unsigned KCS_LEADER = 5;
static unsigned KCS_TRAILER = 5;

int16_t *kcs_encode_sine(unsigned freq,unsigned cycles,unsigned *length){
 const double start_phase = M_PI_2;
 unsigned cycle_length = KCS_FRAMERATE / freq;
 unsigned data_length = cycle_length * cycles;
 unsigned x;
 int16_t *data = NULL;
 
 if(data_length == 0){
  *length = 0;
  data = malloc(1);
  return data;
 }
 
 data = malloc(data_length * sizeof(*data));
 for(x = 0;x < cycle_length;x++){
  /* If amplitude is greater than 1.0, clip the sin wave */
  data[x] = 
   fmax(
    fmin(
     KCS_AMPLITUDE*sin(2 * M_PI * x / cycle_length + start_phase),
     1.0
    ),
    -1.0
   )*INT16_MAX;
 }
 for(;x < data_length;x += cycle_length)
  memcpy(data + x,data,cycle_length * sizeof(*data));
 
 *length = data_length;
 return data;
}

int16_t *kcs_encode_square(unsigned freq,unsigned cycles,unsigned *length){
 unsigned x;
 unsigned cycle_length = KCS_FRAMERATE/freq;
 int16_t *data = NULL;
 unsigned data_length = cycle_length * cycles;
 
 if(data_length == 0){
  *length = 0;
  data = malloc(1);
  return data;
 }
 
 data = malloc(data_length * sizeof(*data));
 for(x = 0;x < cycle_length / 2;x++)
  data[x] = fmin(1.0,fmax(0.0,KCS_AMPLITUDE)) * INT16_MAX;
 for(;x < cycle_length;x++)
  data[x] = 0 - fmin(1.0,fmax(0.0,KCS_AMPLITUDE)) * INT16_MAX;
 for(x = 1;x < cycles;x++)
  memcpy(data + x * cycle_length,data,cycle_length * sizeof(*data));
 *length = data_length;
 return data;
}

static wave_function kcs_encode_wave = kcs_encode_sine;

int16_t *kcs_encode_carrier(unsigned seconds,unsigned *length){
 int16_t *data;
 unsigned data_length;
 
 data = kcs_encode_wave(KCS_ONES_FREQ,seconds*KCS_ONES_FREQ,&data_length);
 (*length) = data_length;
 return data;
}

int16_t *kcs_encode_block(
 char *block,
 unsigned block_length,
 unsigned *length
){
 unsigned x,y,pos = 0;
 int16_t *data = NULL;
 unsigned data_length = 0;
 int16_t *one_pulse;
 unsigned one_length;
 int16_t *zero_pulse;
 unsigned zero_length;
 int16_t *null_pulse;
 unsigned null_length;

 one_pulse = kcs_encode_wave(KCS_ONES_FREQ,KCS_ONES_CYCLES,&one_length);
 zero_pulse = kcs_encode_wave(KCS_ZERO_FREQ,KCS_ZERO_CYCLES,&zero_length);
 null_pulse = kcs_encode_wave(KCS_ONES_FREQ,KCS_NULL_CYCLES,&null_length);
 
 /* Generate data */
 for(y=0;y<block_length;y++){
  
  data = realloc(data,(data_length += zero_length) * sizeof(*data));
  memcpy(data + pos,zero_pulse,zero_length * sizeof(*data));
  pos += zero_length;
  
  for(x = 0x1;x <= 0x80;x <<= 1){
   if(block[y] & x){
    data = realloc(data,(data_length += one_length) * sizeof(*data));
    memcpy(data + pos,one_pulse,one_length * sizeof(*data));
    pos += one_length;
   }else{
    data = realloc(data,(data_length += zero_length) * sizeof(*data));
    memcpy(data + pos,zero_pulse,zero_length * sizeof(*data));
    pos += zero_length;
   }
  }
  
  if(block[y] == '\n' && null_length){
   data = realloc(data,(data_length += null_length) * sizeof(*data));
   memcpy(data + pos,null_pulse,null_length * sizeof(*data));
   pos += null_length;
  }
  
  data = realloc(data,(data_length += one_length * 2) * sizeof(*data));
  memcpy(data + pos,one_pulse,one_length * sizeof(*data));
  pos += one_length;
  memcpy(data + pos,one_pulse,one_length * sizeof(*data));
  pos += one_length;
 }
 
 free(one_pulse);
 free(zero_pulse);
 free(null_pulse);
 *length = data_length;
 return data;
}

void kcs_encode_flac(FILE *ip,char *out){
 FLAC__StreamEncoder *encoder;
 char block[ENC_BLOCKSIZE];
 unsigned block_length,length,x;
 int16_t *buffer;
 FLAC__int32 *pcm;
 
 encoder = FLAC__stream_encoder_new();
 FLAC__stream_encoder_set_channels(encoder,1);
 FLAC__stream_encoder_set_sample_rate(encoder,KCS_FRAMERATE);
 FLAC__stream_encoder_set_bits_per_sample(encoder,sizeof(*buffer)*8);
 FLAC__stream_encoder_set_compression_level(encoder,8);
 FLAC__stream_encoder_init_file(encoder,out,NULL,NULL);
 
 buffer = kcs_encode_carrier(KCS_LEADER,&length);
 pcm = malloc(length*sizeof(*pcm));
 for(x=0;x<length;x++)
  pcm[x] = buffer[x];
 FLAC__stream_encoder_process_interleaved(encoder,pcm,length);
 free(buffer);
 free(pcm);
 
 while(!feof(ip) && !ferror(ip)){
  block_length = fread(block,sizeof(*block),ENC_BLOCKSIZE,ip);
  buffer = kcs_encode_block(block,block_length,&length);
  pcm = malloc(length*sizeof(*pcm));
  for(x=0;x<length;x++)
   pcm[x] = buffer[x];
  FLAC__stream_encoder_process_interleaved(encoder,pcm,length);
  free(buffer);
  free(pcm);
 }
 
 buffer = kcs_encode_carrier(KCS_TRAILER,&length);
 pcm = malloc(length*sizeof(*pcm));
 for(x=0;x<length;x++)
  pcm[x] = buffer[x];
 FLAC__stream_encoder_process_interleaved(encoder,pcm,length);
 free(buffer);
 free(pcm);
 
 FLAC__stream_encoder_finish(encoder);
 FLAC__stream_encoder_delete(encoder);
 
}

void kcs_encode_pa(FILE *ip){
 char block[ENC_BLOCKSIZE];
 int16_t *buffer;
 unsigned block_length,length;
 static pa_sample_spec ss;
 pa_simple *s = NULL;
 int err;
 
 ss.format = PA_SAMPLE_S16LE;
 ss.rate = KCS_FRAMERATE;
 ss.channels = 1;
 
 if(!(s = pa_simple_new(
  NULL,
  "KiloCycleS",
  PA_STREAM_PLAYBACK,
  NULL,
  "Encoding",
  &ss,
  NULL,
  NULL,
  &err
 )))
  goto encode_error;
 
 buffer = kcs_encode_carrier(KCS_LEADER,&length);
 if(pa_simple_write(s,buffer,length * sizeof(*buffer),&err) < 0)
  goto encode_error;
 free(buffer);
 
 while(!ferror(ip) && !feof(ip)){
  block_length = fread(block,1,ENC_BLOCKSIZE,ip);
  buffer = kcs_encode_block(block,block_length,&length);
  if(pa_simple_write(s,buffer,length * sizeof(*buffer),&err) < 0)
   goto encode_error;
  free(buffer);
 }
 
 buffer = kcs_encode_carrier(KCS_TRAILER,&length);
 if(pa_simple_write(s,buffer,length * sizeof(*buffer),&err) < 0)
  goto encode_error;
 free(buffer);
 
 if(pa_simple_drain(s,&err) < 0)
  goto encode_error;
 pa_simple_free(s);
 
 return;
 encode_error:
 fprintf(stderr,"Error: %s\n",pa_strerror(err));
 pa_simple_free(s);
 return;
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

void kcs_decode_flac(FILE *op,char *in){
 FLAC__StreamDecoder *decoder;
 
 decoder = FLAC__stream_decoder_new();
 fputs("Not yet implemented!",stderr);
 
 FLAC__stream_decoder_delete(decoder);
}

void kcs_decode_pa(FILE *op){
 int16_t *data;
 unsigned dec_blocksize = 
  264 * fmax(
   KCS_FRAMERATE * KCS_ONES_CYCLES / KCS_ONES_FREQ,
   KCS_FRAMERATE * KCS_ZERO_CYCLES / KCS_ZERO_FREQ
  );
 char *text;
 unsigned offset = dec_blocksize,text_length = 0;
 static pa_sample_spec ss;
 pa_simple *s = NULL;
 int err;
 
 ss.format = PA_SAMPLE_S16LE;
 ss.rate = KCS_FRAMERATE;
 ss.channels = 1;
 
 data = malloc(dec_blocksize * sizeof(*data));
 if(op == stdout)
  setvbuf(op,NULL,_IONBF,0);
 
 if(!(s = pa_simple_new(
  NULL,
  "KiloCycleS",
  PA_STREAM_RECORD,
  NULL,
  "Decoding",
  &ss,
  NULL,
  NULL,
  &err
 )))
  goto decode_error;
 
 for(;;){
  if(pa_simple_read(s,
   data + dec_blocksize - offset,offset * sizeof(*data),&err) < 0)
   goto decode_error;
  text = kcs_decode_block(data,dec_blocksize,&offset,&text_length);
  fwrite(text,sizeof(*text),text_length,stdout);
  free(text);
  
  memmove(data,data + offset,(dec_blocksize - offset) * sizeof(*data));
 }
 
 pa_simple_free(s);
 
 return;
 decode_error:
 fprintf(stderr,"Error: %s",pa_strerror(err));
 pa_simple_free(s);
 return;
}

int main(int argc,char *argv[]){
 const char *HELP_TEXT = "Type '%s -h' for usage information.\n";
 const char *USAGE_TEXT = (\
"USAGE"\
"  %1$s -h\n"\
"  %1$s [in.txt] [-a 0.8] [-l 5] [-t 5] [-n] -e[f out.flac]\n"\
"  %1$s [out.txt] [-s 0.25] -d[f in.flac]\n"\
"SUMMARY\n"\
"  Encodes text to KCS and vice versa. For more info, see:\n"\
"  http://en.wikipedia.org/wiki/Kansas_City_standard\n"\
"OPTIONS\n"\
" -e\n"\
" -d\n"\
"   Encode or decode (Default: encode)\n"\
" -f\n"\
"   File to use in place of the soundcard.\n"\
"   Can be appended to -e or -d options.\n"\
" -a\n"\
"   Amplitude; for encoding (Default: 0.8)\n"\
" -s\n"\
"   Squelch; for decoding (Default: 0.25)\n"\
" -l\n"\
"   Length of leader in seconds (Default: 5)\n"\
" -t\n"\
"   Length of trailer in seconds (Default: 5)\n"\
" -n\n"\
"   Null pulse cycles, appended to each newline (Default: off)\n"\
" -w\n"\
"   Wave shape; sine or square (Default: sine)\n"\
" -h\n"\
"   Print this info\n"\
"FILES\n"\
"   A text or binary file can be given as a non-option argument on the\n"\
"   command line. If it is not specified, stdin or stdout will be used.\n");
 const char *opts = "hedna:s:l:t:w:f:";
 int opt;
 int encode = 0,decode = 0,help = 0,null_pulse = 0;
 char *flac_io = NULL;
 FILE *fp;
 
 opterr = 0;
 while((opt = getopt(argc,argv,opts)) != -1){
  switch(opt){
   case 'h':
    help = 1;
    encode = decode = 0;
    break;
   case 'e':
    encode = 1;
    break;
   case 'd':
    decode = 1;
    break;
   case 'n':
    null_pulse = 1;
    break;
   case 'a':
    KCS_AMPLITUDE = atof(optarg);
    break;
   case 's':
    KCS_SQUELCH = atof(optarg);
   case 'l':
    KCS_LEADER = atoi(optarg);
    break;
   case 't':
    KCS_TRAILER = atoi(optarg);
    break;
   case 'w':
    if(strcmp(optarg,"square") == 0)
     kcs_encode_wave = kcs_encode_square;
    break;
   case 'f':
    flac_io = optarg;
    break;
   case '?':
    if(strchr(opts,optopt) != NULL)
     fprintf(stderr,"Option -%c requires an argument\n",(char)optopt);
    else
     fprintf(stderr,"Invalid option: -%c\n",(char)optopt);
    fprintf(stderr,HELP_TEXT,argv[0]);
    return 0x1;
   default:
    break;
  }
 }
 if(help){
  fprintf(stderr,USAGE_TEXT,argv[0]);
  return 0;
 }else if(encode && decode){
  fprintf(stderr,"Cannot encode AND decode!\n");
  fprintf(stderr,HELP_TEXT,argv[0]);
  return 2;
 }else if(encode){
  if(!null_pulse)
   KCS_NULL_CYCLES = 0;
  if(optind < argc){
   if((fp = fopen(argv[optind],"rb")) == NULL)
    return 1;
  }else
   fp = stdin;
  if(flac_io != NULL)
   kcs_encode_flac(fp,flac_io);
  else
   kcs_encode_pa(fp);
  fclose(fp);
  return 0;
 }else if(decode){
  if(optind < argc){
   if((fp = fopen(argv[optind],"wb")) == NULL)
    return 1;
  }else
   fp = stdout;
  if(flac_io != NULL)
   kcs_decode_flac(fp,flac_io);
  else
   kcs_decode_pa(fp);
  fclose(fp);
  return 0;
 }else{
  fprintf(stderr,"No arguments given.\n");
  fprintf(stderr,HELP_TEXT,argv[0]);
 }
 return 0;
}
