all: kcs decode_raw sin_generator
clean:
	rm -f kcs decode_raw sin_generator
kcs: kcs.c
	gcc -Wall -s -O2 -o kcs kcs.c `pkg-config --libs --cflags vorbis vorbisenc vorbisfile libpulse-simple flac` -lm
decode_raw: decode_raw.c
	gcc -O2 -o decode_raw decode_raw.c -lm
sin_generator: sin_generator.c
	gcc -o sin_generator sin_generator.c -lm
.PHONY: all clean
