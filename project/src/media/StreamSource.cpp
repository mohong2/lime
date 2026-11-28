#include <media/StreamDecoder.h>

#include <system/CFFI.h>

#include <AL/al.h>
#include <AL/alc.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>


#ifndef LIME_HASHLINK
#ifndef __EMSCRIPTEN__


namespace lime {


	// 8192 frames is ~186 ms at 44.1 kHz stereo. Sixteen blocks give roughly
	// three seconds of slack, which survives a long GC pause or window drag
	// without a dropout while keeping the resident footprint below 600 KB.
	static const int STREAM_BLOCK_FRAMES = 8192;
	static const int STREAM_RING_BUFFERS = 16;
	static const int STREAM_PREBUFFER_BLOCKS = 3;


	enum StreamState {

		STREAM_STATE_STOPPED = 0,
		STREAM_STATE_PLAYING = 1,
		STREAM_STATE_PAUSED = 2,
		STREAM_STATE_FINISHED = 3

	};


	enum CommandType {

		CMD_PLAY,
		CMD_PAUSE,
		CMD_STOP,
		CMD_SEEK,
		CMD_SET_LOOPS,
		CMD_DESTROY

	};


	struct Command {

		CommandType type;
		int64_t frame;
		int loops;
		// Lets a burst of seeks report the newest target: only the command
		// matching the latest serial is allowed to clear seekInFlight.
		int serial;

		Command (CommandType type, int64_t frame = 0, int loops = 0, int serial = 0)
			: type (type), frame (frame), loops (loops), serial (serial) {}

	};


	struct BlockInfo {

		int64_t startFrame;
		int frames;

	};


	static int ALGetInt (ALuint source, ALenum param) {

		ALint value = 0;
		alGetSourcei (source, param, &value);
		return (int)value;

	}


	class StreamSource {

		public:

			StreamSource () {

				alSource = 0;
				decoder = NULL;
				valueRoot = NULL;
				channels = 0;
				frameBytes = 0;
				format = 0;
				sampleRate = 0;
				totalSamples = 0;
				threadStarted = false;
				exiting = false;
				desiredPlaying = false;
				eof = false;
				loopsLeft = 0;
				configuredLoops = 0;
				state = STREAM_STATE_STOPPED;
				seekInFlight = false;
				seekTargetMs = 0;
				seekSerial = 0;
				lastPlayedFrame = 0;
				lastReportedFrame = -1;
				destroyed = false;

				for (int i = 0; i < STREAM_RING_BUFFERS; i++) alBuffers[i] = 0;

			}


			~StreamSource () {}


			bool Initialize (StreamDecoder* decoder) {
				this->decoder = decoder;
				channels = decoder->channels;
				sampleRate = decoder->sampleRate;
				totalSamples = decoder->totalSamples;
				frameBytes = channels * 2;
				format = (channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

				// Clear any stale error left by unrelated audio work.
				while (alGetError () != AL_NO_ERROR) {}

				alGenBuffers (STREAM_RING_BUFFERS, alBuffers);

				if (alGetError () != AL_NO_ERROR) { return false; }

				alGenSources (1, &alSource);

				if (alGetError () != AL_NO_ERROR) { alDeleteBuffers (STREAM_RING_BUFFERS, alBuffers);
					for (int i = 0; i < STREAM_RING_BUFFERS; i++) alBuffers[i] = 0;
					return false;

				}

				for (int i = 0; i < STREAM_RING_BUFFERS; i++) freeBuffers.push_back (alBuffers[i]);

				blockData.resize ((size_t)STREAM_BLOCK_FRAMES * frameBytes);

				try {

					thread = std::thread (&StreamSource::ThreadLoop, this);
					threadStarted = true;
				} catch (...) {

					alDeleteSources (1, &alSource);
					alDeleteBuffers (STREAM_RING_BUFFERS, alBuffers);
					alSource = 0;
					for (int i = 0; i < STREAM_RING_BUFFERS; i++) alBuffers[i] = 0;
					return false;

				}

				return true;

			}


			void Destroy () {

				destroyed = true;

				{

					std::lock_guard<std::mutex> lock (commandMutex);
					exiting = true;

				}

				commandCv.notify_all ();

				if (threadStarted && thread.joinable ()) {

					thread.join ();
					threadStarted = false;

				}

				if (alSource) {

					alSourceStop (alSource);
					alSourcei (alSource, AL_BUFFER, 0);
					alDeleteSources (1, &alSource);
					alSource = 0;

				}

				if (alBuffers[0]) {

					alDeleteBuffers (STREAM_RING_BUFFERS, alBuffers);

					for (int i = 0; i < STREAM_RING_BUFFERS; i++) alBuffers[i] = 0;

				}

				if (decoder) {

					decoder->Close ();
					delete decoder;
					decoder = NULL;

				}

				if (valueRoot) {

					free_root (valueRoot);
					valueRoot = NULL;

				}

			}


			void Play () {

				Push (Command (CMD_PLAY));

			}


			void Pause () {

				Push (Command (CMD_PAUSE));

			}


			void Stop () {

				Push (Command (CMD_STOP));

			}


			void Seek (double fileMs) {

				if (sampleRate <= 0) return;

				double clamped = fileMs;
				if (clamped < 0) clamped = 0;

				int64_t frame = (int64_t)((clamped / 1000.0) * (double)sampleRate);

				if (frame > totalSamples) frame = totalSamples;

				seekTargetMs = (double)frame * 1000.0 / (double)sampleRate;
				seekInFlight = true;

				Push (Command (CMD_SEEK, frame, 0, ++seekSerial));

			}


			void SetLoops (int loops) {

				Push (Command (CMD_SET_LOOPS, 0, loops));

			}


			void SetPitch (float value) {

				if (alSource) alSourcef (alSource, AL_PITCH, value);

			}


			float GetPitch () {

				float value = 1.0f;
				if (alSource) alGetSourcef (alSource, AL_PITCH, &value);
				return value;

			}


			void SetGain (float value) {

				if (alSource) alSourcef (alSource, AL_GAIN, value);

			}


			float GetGain () {

				float value = 1.0f;
				if (alSource) alGetSourcef (alSource, AL_GAIN, &value);
				return value;

			}


			void SetPosition (float x, float y, float z) {

				if (!alSource) return;
				alDistanceModel (AL_NONE);
				alSource3f (alSource, AL_POSITION, x, y, z);

			}


			void GetPosition (float* x, float* y, float* z) {

				if (alSource) {

					alGetSource3f (alSource, AL_POSITION, x, y, z);

				} else {

					*x = 0;
					*y = 0;
					*z = 0;

				}

			}


			int GetState () {

				if (destroyed.load ()) return STREAM_STATE_STOPPED;
				return state.load ();

			}


			double GetTimeMs () {

				if (destroyed.load ()) return 0;
				if (sampleRate <= 0) return 0;

				if (seekInFlight.load ()) return seekTargetMs.load ();

				if (state.load () == STREAM_STATE_FINISHED) {

					return (double)totalSamples * 1000.0 / (double)sampleRate;

				}

				std::lock_guard<std::mutex> lock (queueMutex);

				if (queueInfo.empty ()) {

					return (double)lastPlayedFrame.load () * 1000.0 / (double)sampleRate;

				}

				int offset = ALGetInt (alSource, AL_SAMPLE_OFFSET);

				if (offset < 0) offset = 0;

				// AL_SAMPLE_OFFSET is measured from the first buffer still in
				// the queue: OpenAL Soft's GetSourceOffset adds the length of
				// every buffer that precedes the one currently playing, so the
				// matching base is the oldest queued block. Using the playing
				// block instead (the AL_BUFFERS_PROCESSED index) counts those
				// already played blocks twice and throws the song position
				// forward by a whole block every time the mixer finishes one.
				int64_t sample = queueInfo.front ().startFrame + (int64_t)offset;

				if (sample < 0) sample = 0;
				if (sample > totalSamples) sample = totalSamples;

				// The mixer refreshes the offset once per period, and a stale
				// read or a draining queue can move it back. Never report a
				// smaller position unless the stream really jumped (seek, or a
				// loop wrap), or the chart stutters on the way back.
				if (lastReportedFrame >= 0 && sample < lastReportedFrame && lastReportedFrame - sample <= STREAM_BLOCK_FRAMES) {

					sample = lastReportedFrame;

				}

				lastReportedFrame = sample;

				return (double)sample * 1000.0 / (double)sampleRate;

			}


			// Set by the creation helper before Initialize(). Kept alive for the
			// decoder when the stream reads an in-memory OGG/MP3 image.
			value* valueRoot;


		private:

			void Push (Command command) {

				{

					std::lock_guard<std::mutex> lock (commandMutex);

					if (exiting) return;

					commands.push_back (command);

				}

				commandCv.notify_all ();

			}


			void ThreadLoop () {
				for (;;) {

					std::deque<Command> pending;

					{

						std::unique_lock<std::mutex> lock (commandMutex);

						commandCv.wait_for (lock, std::chrono::milliseconds (5), [this] { return exiting || !commands.empty (); });

						if (exiting && commands.empty ()) break;

						pending.swap (commands);

					}

					bool destroy = false;

					for (size_t i = 0; i < pending.size (); i++) {
						if (Apply (pending[i])) {

							destroy = true;
							break;

						}

					}

					if (destroy || exiting.load ()) break;

					if (desiredPlaying && state.load () != STREAM_STATE_FINISHED) Pump ();

				}
			}


			// Returns true for CMD_DESTROY.
			bool Apply (Command command) {

				switch (command.type) {

					case CMD_PLAY:
					{

						desiredPlaying = true;

						if (state.load () == STREAM_STATE_FINISHED) {

							ResetToFrame (0);

						}

						FillQueue (STREAM_PREBUFFER_BLOCKS);

						{

							std::lock_guard<std::mutex> lock (queueMutex);

							if (!queueInfo.empty ()) {

								if (ALGetInt (alSource, AL_SOURCE_STATE) != AL_PLAYING) {

									alSourcePlay (alSource);

								}

								state = STREAM_STATE_PLAYING;

							} else if (eof) {

								state = STREAM_STATE_FINISHED;
								lastPlayedFrame = totalSamples;

							}

						}

						break;

					}

					case CMD_PAUSE:
					{

						desiredPlaying = false;

						if (alSource && ALGetInt (alSource, AL_SOURCE_STATE) == AL_PLAYING) {

							alSourcePause (alSource);

						}

						if (state.load () != STREAM_STATE_FINISHED) {

							state = STREAM_STATE_PAUSED;

						}

						break;

					}

					case CMD_STOP:
					{

						desiredPlaying = false;
						FlushQueue ();

						if (decoder) decoder->Seek (0);

						eof = false;
						loopsLeft = configuredLoops;
						lastPlayedFrame = 0;
						seekInFlight = false;
						state = STREAM_STATE_STOPPED;
						break;

					}

					case CMD_SEEK:
					{

						int64_t frame = command.frame;
						if (frame < 0) frame = 0;
						if (frame > totalSamples) frame = totalSamples;

						bool wasPlaying = desiredPlaying;

						FlushQueue ();

						if (decoder && !decoder->Seek (frame)) {

							// The decoder refused (corrupt page, truncated file):
							// keep playing from wherever it stopped instead of
							// pretending the seek worked.
							if (command.serial == seekSerial.load ()) seekInFlight = false;
							break;

						}

						eof = false;
						loopsLeft = configuredLoops;
						lastPlayedFrame = frame;

						if (wasPlaying) {

							FillQueue (STREAM_PREBUFFER_BLOCKS);

							std::lock_guard<std::mutex> lock (queueMutex);

							if (!queueInfo.empty ()) {

								alSourcePlay (alSource);
								state = STREAM_STATE_PLAYING;

							} else {

								state = STREAM_STATE_STOPPED;

							}

						} else if (state.load () == STREAM_STATE_FINISHED) {

							state = STREAM_STATE_STOPPED;

						}

						if (command.serial == seekSerial.load ()) seekInFlight = false;
						break;

					}

					case CMD_SET_LOOPS:
					{

						configuredLoops = command.loops;
						loopsLeft = command.loops;

						break;

					}

					case CMD_DESTROY:
					{

						return true;

					}

				}

				return false;

			}


			void ResetToFrame (int64_t frame) {

				FlushQueue ();

				if (decoder) decoder->Seek (frame);

				eof = false;
				loopsLeft = configuredLoops;
				lastPlayedFrame = frame;
				seekInFlight = false;

			}


			void Pump () {

				if (!decoder) return;
				int processed = ALGetInt (alSource, AL_BUFFERS_PROCESSED);
				if (processed > 0) {

					ALuint freed[STREAM_RING_BUFFERS];

					std::lock_guard<std::mutex> lock (queueMutex);

					if (processed > (int)queueInfo.size ()) processed = (int)queueInfo.size ();

					if (processed > 0) {

						alSourceUnqueueBuffers (alSource, processed, freed);

						for (int i = 0; i < processed; i++) {

							BlockInfo info = queueInfo.front ();
							queueInfo.pop_front ();
							lastPlayedFrame = info.startFrame + info.frames;
							freeBuffers.push_back (freed[i]);

						}

					}

				}

				FillQueue (STREAM_RING_BUFFERS);
				int alState = ALGetInt (alSource, AL_SOURCE_STATE);
				if (alState == AL_STOPPED) {

					bool empty = false;

					{

						std::lock_guard<std::mutex> lock (queueMutex);
						empty = queueInfo.empty ();

					}

					if (empty) {

						if (eof) {

							// The queue drained and the decoder has nothing
							// left. This is the only place FINISHED is set.
							state = STREAM_STATE_FINISHED;
							lastPlayedFrame = totalSamples;

						} else if (desiredPlaying) {

							state = STREAM_STATE_FINISHED;
							lastPlayedFrame = totalSamples;

						}

					} else if (desiredPlaying) {

						// OpenAL stops when its queue starves. If the feeder
						// had to refill after that, resume from the queued
						// data instead of leaving a silent source behind.
						alSourcePlay (alSource);

					}

				}

			}


			void FillQueue (int maxQueued) {

				if (!decoder) return;

				while (!eof && freeBuffers.size () > 0 && ALGetInt (alSource, AL_BUFFERS_QUEUED) < maxQueued) {

					if (!FillOne ()) break;

				}

			}


			bool FillOne () {
				ALuint buffer = 0;

				{

					std::lock_guard<std::mutex> lock (queueMutex);

					if (freeBuffers.empty ()) return false;

					buffer = freeBuffers.back ();
					freeBuffers.pop_back ();

				}

				int64_t start = decoder->Tell ();
				int frames = decoder->Read ((int16_t*)blockData.data (), STREAM_BLOCK_FRAMES);
				if (frames <= 0 && loopsLeft > 0) {

					if (decoder->Seek (0)) {

						loopsLeft--;
						eof = false;
						start = 0;
						frames = decoder->Read ((int16_t*)blockData.data (), STREAM_BLOCK_FRAMES);

					}

				}

				if (frames <= 0) {

					eof = true;

					std::lock_guard<std::mutex> lock (queueMutex);
					freeBuffers.push_back (buffer);
					return false;

				}
				alBufferData (buffer, format, blockData.data (), frames * frameBytes, sampleRate);
				{

					std::lock_guard<std::mutex> lock (queueMutex);
					alSourceQueueBuffers (alSource, 1, &buffer);

					BlockInfo info;
					info.startFrame = start;
					info.frames = frames;
					queueInfo.push_back (info);

				}

				if (frames < STREAM_BLOCK_FRAMES) {

					if (loopsLeft > 0 && decoder->Seek (0)) {

						loopsLeft--;
						eof = false;

					} else {

						eof = true;

					}

				}

				return true;

			}


			void FlushQueue () {

				if (!alSource) return;
				alSourceStop (alSource);

				{

					std::lock_guard<std::mutex> lock (queueMutex);

					// Detaching the buffer is the only reliable way to drop
					// buffers that have been queued but not played yet. It
					// also leaves the source ready for a fresh queue.
					alSourcei (alSource, AL_BUFFER, 0);

					queueInfo.clear ();
					freeBuffers.clear ();
					lastReportedFrame = -1;

					for (int i = 0; i < STREAM_RING_BUFFERS; i++) freeBuffers.push_back (alBuffers[i]);

				}
			}


			ALuint alSource;
			ALuint alBuffers[STREAM_RING_BUFFERS];
			StreamDecoder* decoder;
			int channels;
			int frameBytes;
			int format;
			int sampleRate;
			int64_t totalSamples;
			std::vector<uint8_t> blockData;
			std::vector<ALuint> freeBuffers;
			std::deque<BlockInfo> queueInfo;

			std::atomic<bool> destroyed;

			std::mutex commandMutex;
			std::condition_variable commandCv;
			std::deque<Command> commands;
			std::thread thread;
			bool threadStarted;
			std::atomic<bool> exiting;

			std::mutex queueMutex;
			std::atomic<int> state;
			std::atomic<bool> seekInFlight;
			std::atomic<double> seekTargetMs;
			std::atomic<int> seekSerial;
			std::atomic<int64_t> lastPlayedFrame;

			// Guarded by queueMutex: position reported by the previous
			// GetTimeMs call, used to keep the reported time monotonic.
			int64_t lastReportedFrame;

			// Feeder-thread only.
			bool desiredPlaying;
			bool eof;
			int loopsLeft;
			int configuredLoops;

	};


	// Handles stay small: the feeder thread owns the AL objects, so Haxe only
	// ever passes an integer around and no native pointer can outlive the
	// registry entry.
	static std::mutex sRegistryMutex;
	// shared_ptr keeps a source alive while another thread is still inside a
	// query (the Haxe timer polls time every 25 ms, dispose can race it).
	static std::map<int, std::shared_ptr<StreamSource>> sSources;
	static int sNextSourceId = 1;


	static std::shared_ptr<StreamSource> LookupSource (int id) {

		std::lock_guard<std::mutex> lock (sRegistryMutex);
		std::map<int, std::shared_ptr<StreamSource>>::iterator it = sSources.find (id);

		if (it == sSources.end ()) return std::shared_ptr<StreamSource> ();

		return it->second;

	}


}


extern "C" {


	static value lime_stream_set_info (value audioBuffer, int channels, int sampleRate, int64_t totalSamples) {

		static int id_bitsPerSample = -1;
		static int id_channels = -1;
		static int id_sampleRate = -1;
		static int id_totalSamples = -1;

		if (id_bitsPerSample < 0) {

			id_bitsPerSample = val_id ("bitsPerSample");
			id_channels = val_id ("channels");
			id_sampleRate = val_id ("sampleRate");
			id_totalSamples = val_id ("__streamTotalSamples");

		}

		alloc_field (audioBuffer, id_bitsPerSample, alloc_int (16));
		alloc_field (audioBuffer, id_channels, alloc_int (channels));
		alloc_field (audioBuffer, id_sampleRate, alloc_int (sampleRate));
		alloc_field (audioBuffer, id_totalSamples, alloc_int ((int)totalSamples));

		return audioBuffer;

	}


	static const uint8_t* StreamValueBytes (value data, int* outLength) {

		static int id_length = -1;
		static int id_b = -1;

		if (id_length < 0) {

			id_length = val_id ("length");
			id_b = val_id ("b");

		}

		int length = val_int (val_field (data, id_length));

		if (length <= 0) {

			*outLength = 0;
			return NULL;

		}

		value _b = val_field (data, id_b);
		const uint8_t* bytes;

		if (val_is_string (_b)) {

			bytes = (const uint8_t*)val_string (_b);

		} else {

			bytes = (const uint8_t*)buffer_data (val_to_buffer (_b));

		}

		*outLength = length;
		return bytes;

	}


	static value lime_stream_probe_bytes (value data, value audioBuffer) {

		if (val_is_null (data) || val_is_null (audioBuffer)) return alloc_null ();

		int length = 0;
		const uint8_t* bytes = StreamValueBytes (data, &length);

		if (!bytes) return alloc_null ();

		int channels, sampleRate;
		int64_t totalSamples;

		if (!lime::StreamProbeMemory (bytes, (size_t)length, &channels, &sampleRate, &totalSamples)) {

			return alloc_null ();

		}

		return lime_stream_set_info (audioBuffer, channels, sampleRate, totalSamples);

	}


	static value lime_stream_probe_file (value path, value audioBuffer) {

		if (val_is_null (path) || val_is_null (audioBuffer)) return alloc_null ();

		int channels, sampleRate;
		int64_t totalSamples;

		if (!lime::StreamProbeFile (val_string (path), &channels, &sampleRate, &totalSamples)) {

			return alloc_null ();

		}

		return lime_stream_set_info (audioBuffer, channels, sampleRate, totalSamples);

	}


	static int lime_stream_source_create_common (lime::StreamDecoder* decoder, value bytesValue) {

		if (!decoder) return -1;

		lime::StreamSource* source = new lime::StreamSource ();
		source->valueRoot = NULL;

		if (!val_is_null (bytesValue)) {

			value* root = alloc_root ();
			*root = bytesValue;
			source->valueRoot = root;

		}

		if (!source->Initialize (decoder)) {

			if (source->valueRoot) {

				free_root (source->valueRoot);
				source->valueRoot = NULL;

			}

			delete source;
			return -1;

		}

		std::lock_guard<std::mutex> lock (lime::sRegistryMutex);

		int id = lime::sNextSourceId++;
		lime::sSources[id] = std::shared_ptr<lime::StreamSource> (source);

		return id;

	}


	static int lime_stream_source_create_file (value path) {

		if (val_is_null (path)) return -1;

		lime::StreamDecoder* decoder = lime::StreamOpenFileDecoder (val_string (path));

		return lime_stream_source_create_common (decoder, alloc_null ());

	}


	static int lime_stream_source_create_bytes (value data) {

		if (val_is_null (data)) return -1;

		int length = 0;
		const uint8_t* bytes = StreamValueBytes (data, &length);

		if (!bytes) return -1;

		lime::StreamDecoder* decoder = lime::StreamOpenMemoryDecoder (bytes, (size_t)length);

		if (!decoder) return -1;

		return lime_stream_source_create_common (decoder, data);

	}


	static void lime_stream_source_destroy (int id) {

		std::shared_ptr<lime::StreamSource> source;

		{

			std::lock_guard<std::mutex> lock (lime::sRegistryMutex);

			std::map<int, std::shared_ptr<lime::StreamSource>>::iterator it = lime::sSources.find (id);

			if (it == lime::sSources.end ()) return;

			source = it->second;
			lime::sSources.erase (it);

		}

		// Any concurrent query holds its own reference, so the object outlives
		// the registry entry; Destroy () is only reached once.
		if (source) source->Destroy ();

	}


	static void lime_stream_source_play (int handle) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (source) source->Play ();

	}


	static void lime_stream_source_pause (int handle) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (source) source->Pause ();

	}


	static void lime_stream_source_stop (int handle) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (source) source->Stop ();

	}


	static void lime_stream_source_seek (int handle, double fileMs) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (source) source->Seek (fileMs);

	}


	static void lime_stream_source_set_loops (int handle, double loops) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (source) source->SetLoops ((int)loops);

	}


	static void lime_stream_source_set_pitch (int handle, double pitch) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (source) source->SetPitch ((float)pitch);

	}


	static void lime_stream_source_set_gain (int handle, double gain) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (source) source->SetGain ((float)gain);

	}


	static void lime_stream_source_set_position (int handle, double x, double y, double z) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);

		if (source) source->SetPosition ((float)x, (float)y, (float)z);

	}


	static double lime_stream_source_get_time (int handle) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (!source) return 0;
		return source->GetTimeMs ();

	}


	static int lime_stream_source_get_state (int handle) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (!source) return 0;
		return source->GetState ();

	}


	static double lime_stream_source_get_pitch (int handle) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (!source) return 1;
		return source->GetPitch ();

	}


	static double lime_stream_source_get_gain (int handle) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);
		if (!source) return 1;
		return source->GetGain ();

	}


	static value lime_stream_source_get_position (int handle, value out) {

		std::shared_ptr<lime::StreamSource> source = lime::LookupSource (handle);

		if (!source || val_is_null (out)) return alloc_null ();

		static int id_x = val_id ("x");
		static int id_y = val_id ("y");
		static int id_z = val_id ("z");

		float x = 0, y = 0, z = 0;
		source->GetPosition (&x, &y, &z);

		alloc_field (out, id_x, alloc_float (x));
		alloc_field (out, id_y, alloc_float (y));
		alloc_field (out, id_z, alloc_float (z));

		return out;

	}


	DEFINE_PRIME2 (lime_stream_probe_bytes);
	DEFINE_PRIME2 (lime_stream_probe_file);
	DEFINE_PRIME1 (lime_stream_source_create_file);
	DEFINE_PRIME1 (lime_stream_source_create_bytes);
	DEFINE_PRIME1v (lime_stream_source_destroy);
	DEFINE_PRIME1v (lime_stream_source_play);
	DEFINE_PRIME1v (lime_stream_source_pause);
	DEFINE_PRIME1v (lime_stream_source_stop);
	DEFINE_PRIME2v (lime_stream_source_seek);
	DEFINE_PRIME2v (lime_stream_source_set_loops);
	DEFINE_PRIME2v (lime_stream_source_set_pitch);
	DEFINE_PRIME2v (lime_stream_source_set_gain);
	DEFINE_PRIME4v (lime_stream_source_set_position);
	DEFINE_PRIME1 (lime_stream_source_get_time);
	DEFINE_PRIME1 (lime_stream_source_get_state);
	DEFINE_PRIME1 (lime_stream_source_get_pitch);
	DEFINE_PRIME1 (lime_stream_source_get_gain);
	DEFINE_PRIME2 (lime_stream_source_get_position);


}


#endif // __EMSCRIPTEN__
#endif // LIME_HASHLINK
