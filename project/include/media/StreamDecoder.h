#ifndef LIME_STREAM_DECODER_H
#define LIME_STREAM_DECODER_H

#include <stdint.h>
#include <stddef.h>


namespace lime {


	enum StreamFormat {

		STREAM_FORMAT_NONE = 0,
		STREAM_FORMAT_VORBIS = 1,
		STREAM_FORMAT_MP3 = 2

	};


	/**
	 * Pull decoder for the streamed audio path. Implementations always output
	 * interleaved signed 16-bit PCM in the platform's native byte order, which
	 * is exactly what the static (fully decoded) path hands to OpenAL.
	 */
	class StreamDecoder {

		public:

			int channels;
			int sampleRate;
			int64_t totalSamples;

			StreamDecoder () : channels (0), sampleRate (0), totalSamples (-1) {}
			virtual ~StreamDecoder () {}

			virtual bool OpenFile (const char* path) = 0;
			virtual bool OpenMemory (const uint8_t* data, size_t size) = 0;
			// Reads at most maxFrames frames. Returns the number of frames read;
			// 0 means end of stream (or an unrecoverable decode error).
			virtual int Read (int16_t* dst, int maxFrames) = 0;
			virtual bool Seek (int64_t frame) = 0;
			virtual int64_t Tell () = 0;
			virtual void Close () = 0;

	};


	StreamFormat StreamSniff (const uint8_t* data, size_t size);
	StreamDecoder* OpenStreamDecoder (StreamFormat format);

	// Convenience helpers used by StreamSource: they sniff the container and
	// hand back a ready decoder, so the calling thread performs no file I/O of
	// its own beyond opening the handle.
	StreamDecoder* StreamOpenFileDecoder (const char* path);
	StreamDecoder* StreamOpenMemoryDecoder (const uint8_t* data, size_t size);

	bool StreamProbeFile (const char* path, int* channels, int* sampleRate, int64_t* totalSamples);
	bool StreamProbeMemory (const uint8_t* data, size_t size, int* channels, int* sampleRate, int64_t* totalSamples);


}


#endif // LIME_STREAM_DECODER_H
