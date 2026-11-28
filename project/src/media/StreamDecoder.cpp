#include <media/StreamDecoder.h>

#include <system/System.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif


// Deliberately plain CRT I/O: the feeder thread that drives these decoders is
// not an hxcpp thread, and lime::fread/fseek bracket every call with
// System::GCEnterBlocking (), which is only valid for hxcpp threads. Standard
// I/O keeps the decoder self-contained and thread safe.
static FILE* StreamOpenFile (const char* path) {

	if (!path) return NULL;

	#ifdef _WIN32

	int length = MultiByteToWideChar (CP_UTF8, 0, path, -1, NULL, 0);

	if (length > 0) {

		std::vector<wchar_t> wide ((size_t)length);

		if (MultiByteToWideChar (CP_UTF8, 0, path, -1, wide.data (), length) == length) {

			FILE* file = ::_wfopen (wide.data (), L"rb");
			if (file) return file;

		}

	}

	#endif

	return ::fopen (path, "rb");

}


#ifdef LIME_VORBIS
	#include <vorbis/vorbisfile.h>
#endif

// The header vendors its own implementation; only this translation unit
// defines DR_MP3_IMPLEMENTATION.
#include "dr_mp3.h"


namespace lime {


	// --- Vorbis -----------------------------------------------------------------


#ifdef LIME_VORBIS

	struct VorbisMemoryCursor {

		const uint8_t* data;
		size_t size;
		size_t pos;

	};


	static size_t VorbisMemoryRead (void* ptr, size_t size, size_t count, void* datasource) {

		VorbisMemoryCursor* cursor = (VorbisMemoryCursor*)datasource;
		size_t wanted = size * count;
		size_t available = cursor->size - cursor->pos;

		if (wanted > available) wanted = available;
		if (wanted > 0) {

			memcpy (ptr, cursor->data + cursor->pos, wanted);
			cursor->pos += wanted;

		}

		return size ? wanted / size : 0;

	}


	static int VorbisMemorySeek (void* datasource, ogg_int64_t offset, int whence) {

		VorbisMemoryCursor* cursor = (VorbisMemoryCursor*)datasource;

		switch (whence) {

			case SEEK_SET: cursor->pos = (size_t)offset; break;
			case SEEK_CUR: cursor->pos += (size_t)offset; break;
			case SEEK_END: cursor->pos = cursor->size + (size_t)offset; break;
			default: return -1;

		}

		if (cursor->pos > cursor->size) cursor->pos = cursor->size;
		return 0;

	}


	static long VorbisMemoryTell (void* datasource) {

		return (long)((VorbisMemoryCursor*)datasource)->pos;

	}


	static int VorbisMemoryClose (void* datasource) {

		return 0;

	}


	static ov_callbacks VORBIS_MEMORY_CALLBACKS = {

		VorbisMemoryRead,
		VorbisMemorySeek,
		VorbisMemoryClose,
		VorbisMemoryTell

	};


	static size_t VorbisFileRead (void* ptr, size_t size, size_t count, void* datasource) {

		return ::fread (ptr, size, count, (FILE*)datasource);

	}


	static int VorbisFileSeek (void* datasource, ogg_int64_t offset, int whence) {

		return ::fseek ((FILE*)datasource, (long)offset, whence);

	}


	static int VorbisFileClose (void* datasource) {

		return ::fclose ((FILE*)datasource);

	}


	static long VorbisFileTell (void* datasource) {

		return ::ftell ((FILE*)datasource);

	}


	static ov_callbacks VORBIS_FILE_CALLBACKS = {

		VorbisFileRead,
		VorbisFileSeek,
		VorbisFileClose,
		VorbisFileTell

	};


	class VorbisStreamDecoder : public StreamDecoder {

		public:

			VorbisStreamDecoder () {

				memset (&file, 0, sizeof (file));
				opened = false;
				handle = NULL;
				cursor = NULL;
				errors = 0;

			}


			~VorbisStreamDecoder () {

				Close ();

			}


			bool OpenFile (const char* path) override {

				Close ();

				handle = StreamOpenFile (path);
				if (!handle) { return false; }

				memset (&file, 0, sizeof (file));

				if (ov_open_callbacks (handle, &file, NULL, 0, VORBIS_FILE_CALLBACKS) != 0) {
					::fclose (handle);
					handle = NULL;
					return false;

				}

				opened = true;
				return ReadInfo ();

			}


			bool OpenMemory (const uint8_t* data, size_t size) override {

				Close ();

				cursor = new VorbisMemoryCursor ();
				cursor->data = data;
				cursor->size = size;
				cursor->pos = 0;

				memset (&file, 0, sizeof (file));

				if (ov_open_callbacks (cursor, &file, NULL, 0, VORBIS_MEMORY_CALLBACKS) != 0) {

					delete cursor;
					cursor = NULL;
					return false;

				}

				opened = true;
				return ReadInfo ();

			}


			int Read (int16_t* dst, int maxFrames) override {

				if (!opened || maxFrames <= 0) return 0;

				int frameBytes = channels * 2;
				int maxBytes = maxFrames * frameBytes;
				int total = 0;

				while (total < maxBytes) {

					int bitStream = 0;
					long got = ov_read (&file, (char*)dst + total, maxBytes - total, 0, 2, 1, &bitStream);

					if (got > 0) {

						total += (int)got;
						errors = 0;

					} else if (got == 0) {

						break;

					} else if (++errors > 8) {

						break;

					}

				}

				return total / frameBytes;

			}


			bool Seek (int64_t frame) override {

				if (!opened) return false;
				return ov_pcm_seek (&file, frame) == 0;

			}


			int64_t Tell () override {

				if (!opened) return 0;
				return (int64_t)ov_pcm_tell (&file);

			}


			void Close () override {

				if (opened) {

					ov_clear (&file);
					opened = false;
					handle = NULL;

				} else if (handle) {

					::fclose (handle);
					handle = NULL;

				}

				if (cursor) {

					delete cursor;
					cursor = NULL;

				}

			}


		private:

			bool ReadInfo () {

				vorbis_info* info = ov_info (&file, -1);

				if (!info || (info->channels != 1 && info->channels != 2)) {
					Close ();
					return false;

				}

				channels = info->channels;
				sampleRate = (int)info->rate;
				totalSamples = (int64_t)ov_pcm_total (&file, -1);
				if (sampleRate <= 0 || totalSamples <= 0) {
					Close ();
					return false;

				}

				return true;

			}


			OggVorbis_File file;
			FILE* handle;
			VorbisMemoryCursor* cursor;
			bool opened;
			int errors;

	};

#endif // LIME_VORBIS


	// --- MP3 --------------------------------------------------------------------


	static size_t Mp3FileRead (void* userData, void* buffer, size_t bytesToRead) {

		return ::fread (buffer, 1, bytesToRead, (FILE*)userData);

	}


	static drmp3_bool32 Mp3FileSeek (void* userData, int offset, drmp3_seek_origin origin) {

		return ::fseek ((FILE*)userData, (long)offset, (int)origin) == 0;

	}


	class Mp3StreamDecoder : public StreamDecoder {

		public:

			Mp3StreamDecoder () {

				memset (&mp3, 0, sizeof (mp3));
				opened = false;
				handle = NULL;

			}


			~Mp3StreamDecoder () {

				Close ();

			}


			bool OpenFile (const char* path) override {

				Close ();

				handle = StreamOpenFile (path);
				if (!handle) return false;

				memset (&mp3, 0, sizeof (mp3));

				if (!drmp3_init (&mp3, Mp3FileRead, Mp3FileSeek, handle, NULL)) {

					::fclose (handle);
					handle = NULL;
					return false;

				}

				opened = true;
				return ReadInfo ();

			}


			bool OpenMemory (const uint8_t* data, size_t size) override {

				Close ();

				memset (&mp3, 0, sizeof (mp3));

				if (!drmp3_init_memory (&mp3, data, size, NULL)) return false;

				opened = true;
				return ReadInfo ();

			}


			int Read (int16_t* dst, int maxFrames) override {

				if (!opened || maxFrames <= 0) return 0;

				drmp3_uint64 frames = drmp3_read_pcm_frames_s16 (&mp3, (drmp3_uint64)maxFrames, (drmp3_int16*)dst);
				return (int)frames;

			}


			bool Seek (int64_t frame) override {

				if (!opened) return false;
				return drmp3_seek_to_pcm_frame (&mp3, (drmp3_uint64)frame);

			}


			int64_t Tell () override {

				if (!opened) return 0;
				return (int64_t)mp3.currentPCMFrame;

			}


			void Close () override {

				if (opened) {

					drmp3_uninit (&mp3);
					opened = false;

				}

				if (handle) {

					::fclose (handle);
					handle = NULL;

				}

			}


		private:

			bool ReadInfo () {

				if (mp3.channels != 1 && mp3.channels != 2) {

					Close ();
					return false;

				}

				channels = mp3.channels;
				sampleRate = (int)mp3.sampleRate;
				totalSamples = (int64_t)drmp3_get_pcm_frame_count (&mp3);

				if (sampleRate <= 0 || totalSamples <= 0) {

					Close ();
					return false;

				}

				return true;

			}


			drmp3 mp3;
			FILE* handle;
			bool opened;

	};


	// --- Shared -----------------------------------------------------------------


	StreamFormat StreamSniff (const uint8_t* data, size_t size) {

		if (!data || size < 4) return STREAM_FORMAT_NONE;

		if (data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S') {

			return STREAM_FORMAT_VORBIS;

		}

		if (size >= 3 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {

			return STREAM_FORMAT_MP3;

		}

		if (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {

			return STREAM_FORMAT_MP3;

		}

		return STREAM_FORMAT_NONE;

	}


	StreamDecoder* OpenStreamDecoder (StreamFormat format) {

		switch (format) {

			case STREAM_FORMAT_VORBIS:
				#ifdef LIME_VORBIS
				return new VorbisStreamDecoder ();
				#else
				return NULL;
				#endif

			case STREAM_FORMAT_MP3:
				return new Mp3StreamDecoder ();

			default:
				return NULL;

		}

	}


	static bool ReadFileHead (const char* path, uint8_t* head, size_t headSize, size_t* outSize) {

		FILE* handle = StreamOpenFile (path);
		if (!handle) return false;

		*outSize = ::fread (head, 1, headSize, handle);
		::fclose (handle);
		return true;

	}


	bool StreamProbeFile (const char* path, int* channels, int* sampleRate, int64_t* totalSamples) {

		if (channels) *channels = 0;
		if (sampleRate) *sampleRate = 0;
		if (totalSamples) *totalSamples = 0;
		if (!path) return false;

		uint8_t head[64];
		size_t headSize = 0;

		if (!ReadFileHead (path, head, sizeof (head), &headSize)) { return false; }

		StreamFormat format = StreamSniff (head, headSize);
		StreamDecoder* decoder = OpenStreamDecoder (format);
		if (!decoder) return false;
		if (!decoder->OpenFile (path)) {
			delete decoder;
			return false;

		}

		if (channels) *channels = decoder->channels;
		if (sampleRate) *sampleRate = decoder->sampleRate;
		if (totalSamples) *totalSamples = decoder->totalSamples;

		decoder->Close ();
		delete decoder;
		return true;

	}


	bool StreamProbeMemory (const uint8_t* data, size_t size, int* channels, int* sampleRate, int64_t* totalSamples) {

		if (channels) *channels = 0;
		if (sampleRate) *sampleRate = 0;
		if (totalSamples) *totalSamples = 0;
		if (!data) return false;

		StreamFormat format = StreamSniff (data, size);
		StreamDecoder* decoder = OpenStreamDecoder (format);

		if (!decoder) return false;
		if (!decoder->OpenMemory (data, size)) {

			delete decoder;
			return false;

		}

		if (channels) *channels = decoder->channels;
		if (sampleRate) *sampleRate = decoder->sampleRate;
		if (totalSamples) *totalSamples = decoder->totalSamples;

		decoder->Close ();
		delete decoder;
		return true;

	}


	StreamDecoder* StreamOpenFileDecoder (const char* path) {

		if (!path) return NULL;

		uint8_t head[64];
		size_t headSize = 0;

		if (!ReadFileHead (path, head, sizeof (head), &headSize)) return NULL;

		StreamDecoder* decoder = OpenStreamDecoder (StreamSniff (head, headSize));

		if (!decoder) return NULL;

		if (!decoder->OpenFile (path)) {

			delete decoder;
			return NULL;

		}

		return decoder;

	}


	StreamDecoder* StreamOpenMemoryDecoder (const uint8_t* data, size_t size) {

		if (!data || size == 0) return NULL;

		StreamDecoder* decoder = OpenStreamDecoder (StreamSniff (data, size));

		if (!decoder) return NULL;

		if (!decoder->OpenMemory (data, size)) {

			delete decoder;
			return NULL;

		}

		return decoder;

	}


}
