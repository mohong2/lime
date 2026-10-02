#include <ui/FileDialog.h>
#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(LIME_SDL3) && !defined(HX_MACOS)

#include <codecvt>
#include <locale>
#include <SDL3/SDL.h>

#else

#include <tinyfiledialogs.h>

#endif


namespace lime {


	std::string* wstring_to_string (std::wstring* source) {

		if (!source) return NULL;

		int size = std::wcslen (source->c_str ());
		char* temp = (char*)malloc (size + 1);
		std::wcstombs (temp, source->c_str (), size);
		temp[size] = '\0';

		std::string* data = new std::string (temp);
		free (temp);
		return data;

	}

#if defined(LIME_SDL3) && !defined(HX_MACOS)

	// ---- SDL3 backend (LIME_SDL3 && !HX_MACOS) ----
	//
	// lime 的 FileDialog::* 契约是「调用返回即结果」：Haxe 侧在 BackgroundWorker 工作线程上
	// 调用这些 FFI 函数（src/lime/ui/FileDialog.hx + src/lime/system/BackgroundWorker.hx）。
	// SDL3 的 SDL_ShowFileDialogWithProperties 是异步回调式的，因此这里发起请求后在条件变量上
	// 等待回调，把异步结果收敛回同一个同步契约——Haxe / FFI / 上层调用方零改动。

	struct SDLFileDialogState {

		SDL_Mutex* mutex;
		SDL_Condition* condition;
		bool completed;
		bool error;
		std::vector<std::string> patterns;
		std::vector<SDL_DialogFileFilter> filters;
		std::vector<std::string> files;

	};


	// 转换失败时的字节级退化路径（与 ExternalInterface.cpp 非 Windows 分支的语义一致）。
	// 显式 cast 避免 MSVC C4244（wchar_t -> char 窄化）警告。
	static std::string wstring_byte_fallback (const std::wstring& source) {

		std::string result;
		result.reserve (source.size ());

		for (std::size_t i = 0; i < source.size (); i++) {

			result.push_back ((char)source[i]);

		}

		return result;

	}


	static std::string wstring_to_utf8 (std::wstring* source) {

		if (!source) return std::string ();

		#ifdef HX_WINDOWS

		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;

		try {

			return converter.to_bytes (*source);

		} catch (...) {

			return wstring_byte_fallback (*source);

		}

		#else

		return wstring_byte_fallback (*source);

		#endif

	}


	static std::wstring* utf8_to_wstring (const char* source) {

		if (!source) return 0;

		std::string _source = std::string (source);

		#ifdef HX_WINDOWS

		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;

		try {

			return new std::wstring (converter.from_bytes (_source));

		} catch (...) {

			return new std::wstring (_source.begin (), _source.end ());

		}

		#else

		return new std::wstring (_source.begin (), _source.end ());

		#endif

	}


	// lime 的 filter 参数是逗号/分号分隔的扩展名串（openfl 传的是分号分隔，可能带 "*." 前缀）。
	// SDL 只接受 [a-zA-Z0-9_.-] 组成的分号列表，或整串单个 "*"（SDL_dialog_utils.c:231-256）。
	static void parse_filters (std::wstring* filter, std::vector<std::string>& patterns) {

		std::string raw = wstring_to_utf8 (filter);
		std::string token;

		for (std::size_t i = 0; i <= raw.size (); i++) {

			char c = (i < raw.size ()) ? raw[i] : ',';

			if (c != ',' && c != ';') {

				token.push_back (c);
				continue;

			}

			std::size_t start = 0;
			std::size_t end = token.size ();

			while (start < end && (token[start] == ' ' || token[start] == '\t')) start++;
			while (end > start && (token[end - 1] == ' ' || token[end - 1] == '\t')) end--;

			std::string pattern = token.substr (start, end - start);

			token.clear ();

			if (pattern.size () == 0) continue;

			if (pattern == "*") {

				// 单个 "*" 表示全部文件：SDL 要求它必须是整串，不能与其他 pattern 混用
				patterns.clear ();
				return;

			}

			// 去掉 "*." / "*" / "." 前缀（单独的 "*" 已在上方按全部文件处理）
			while (pattern.size () > 0 && (pattern[0] == '*' || pattern[0] == '.')) {

				pattern = pattern.substr (1);

			}

			if (pattern.size () > 0) patterns.push_back (pattern);

		}

	}


	static void init_state (SDLFileDialogState* state, std::wstring* filter) {

		state->mutex = 0;
		state->condition = 0;
		state->completed = false;
		state->error = false;

		parse_filters (filter, state->patterns);

		// SDL_DialogFileFilter::pattern 必须活到回调触发（SDL_dialog.h:142-145），
		// 因此指向 state->patterns 内部、且此后不再修改该 vector。
		// name 必须非 NULL：SDL 的 Windows/zenity/cocoa 后端都会 SDL_strdup(filter.name)。
		for (std::size_t i = 0; i < state->patterns.size (); i++) {

			SDL_DialogFileFilter entry;
			entry.name = "Files";
			entry.pattern = state->patterns[i].c_str ();
			state->filters.push_back (entry);

		}

	}


	// filelist 三态（SDL_dialog.h:81-88）：NULL = 出错，指向 NULL = 取消，非 NULL = 路径列表。
	// filelist 在回调返回后被释放（SDL_dialog.h:90-91），所以这里立刻拷贝。
	static void SDLCALL SDLFileDialogCallback (void* userdata, const char* const* filelist, int filter) {

		SDLFileDialogState* state = (SDLFileDialogState*)userdata;

		if (!filelist) {

			state->error = true;

		} else {

			for (int i = 0; filelist[i] != NULL; i++) {

				state->files.push_back (std::string (filelist[i]));

			}

		}

		SDL_LockMutex (state->mutex);
		state->completed = true;
		SDL_SignalCondition (state->condition);
		SDL_UnlockMutex (state->mutex);

	}


	static void show_file_dialog (SDL_FileDialogType type, SDLFileDialogState* state, std::wstring* title, std::wstring* defaultPath, bool allowMany) {

		state->mutex = SDL_CreateMutex ();
		state->condition = SDL_CreateCondition ();

		SDL_PropertiesID props = SDL_CreateProperties ();

		if (state->filters.size () > 0) {

			SDL_SetPointerProperty (props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void*)state->filters.data ());
			SDL_SetNumberProperty (props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, (Sint64)state->filters.size ());

		}

		if (allowMany) {

			SDL_SetBooleanProperty (props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, true);

		}

		if (title) {

			std::string _title = wstring_to_utf8 (title);
			SDL_SetStringProperty (props, SDL_PROP_FILE_DIALOG_TITLE_STRING, _title.c_str ());

		}

		if (defaultPath) {

			std::string _defaultPath = wstring_to_utf8 (defaultPath);
			SDL_SetStringProperty (props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, _defaultPath.c_str ());

		}

		// 父窗口保持 NULL：与 tinyfd 现状（ofn.hwndOwner = 0）一致，避免引入 SDLWindow 依赖
		SDL_ShowFileDialogWithProperties (type, SDLFileDialogCallback, state, props);

		SDL_DestroyProperties (props);

		// 参数校验等错误路径会在上面的调用返回前同步触发回调（SDL_dialog.c:33/40/48/61），
		// 因此用 completed 标志轮询，而不是「调用之后再等一次信号」。
		SDL_LockMutex (state->mutex);

		while (!state->completed) {

			SDL_WaitCondition (state->condition, state->mutex);

		}

		SDL_UnlockMutex (state->mutex);

		SDL_DestroyCondition (state->condition);
		SDL_DestroyMutex (state->mutex);

		state->condition = 0;
		state->mutex = 0;

	}

#endif


	std::wstring* FileDialog::OpenDirectory (std::wstring* title, std::wstring* filter, std::wstring* defaultPath) {

#if defined(LIME_SDL3) && !defined(HX_MACOS)

		SDLFileDialogState state;

		// 目录选择不使用 filter：与 tinyfd 现状一致（原实现同样是 // TODO: Filter?）
		init_state (&state, 0);

		show_file_dialog (SDL_FILEDIALOG_OPENFOLDER, &state, title, defaultPath, false);

		if (state.error || state.files.size () == 0) {

			return 0;

		}

		return utf8_to_wstring (state.files[0].c_str ());

#else


		// TODO: Filter?

		#ifdef HX_WINDOWS

		const wchar_t* path = tinyfd_selectFolderDialogW (title ? title->c_str () : 0, defaultPath ? defaultPath->c_str () : 0);

		if (path && std::wcslen(path) > 0) {

			std::wstring* _path = new std::wstring (path);
			return _path;

		}

		#else

		std::string* _title = wstring_to_string (title);
		//std::string* _filter = wstring_to_string (filter);
		std::string* _defaultPath = wstring_to_string (defaultPath);

		const char* path = tinyfd_selectFolderDialog (_title ? _title->c_str () : NULL, _defaultPath ? _defaultPath->c_str () : NULL);

		if (_title) delete _title;
		//if (_filter) delete _filter;
		if (_defaultPath) delete _defaultPath;

		if (path && std::strlen(path) > 0) {

			std::string _path = std::string (path);
			std::wstring* __path = new std::wstring (_path.begin (), _path.end ());
			return __path;

		}

		#endif

		return 0;

#endif

	}


	std::wstring* FileDialog::OpenFile (std::wstring* title, std::wstring* filter, std::wstring* defaultPath) {

#if defined(LIME_SDL3) && !defined(HX_MACOS)

		SDLFileDialogState state;

		init_state (&state, filter);

		show_file_dialog (SDL_FILEDIALOG_OPENFILE, &state, title, defaultPath, false);

		if (state.error || state.files.size () == 0) {

			return 0;

		}

		return utf8_to_wstring (state.files[0].c_str ());

#else


		#ifdef HX_WINDOWS

		std::vector<std::wstring> filters_vec;
		if (filter) {
			std::wstring temp (L"*.");
			std::wstring line;
			std::wstringstream ss(*filter);
			while(std::getline(ss, line, L',')) {
				filters_vec.push_back(temp + line);
			}
		}

		const int numFilters = filter ? filters_vec.size() : 1;
		const wchar_t **filters = new const wchar_t*[numFilters];
		if (filter && numFilters > 0) {
			for (int index = 0; index < numFilters; index++) {
				filters[index] = const_cast<wchar_t*>(filters_vec[index].c_str());
			}
		} else {
			filters[0] = NULL;
		}

		const wchar_t* path = tinyfd_openFileDialogW (title ? title->c_str () : 0, defaultPath ? defaultPath->c_str () : 0, filter ? numFilters : 0, filter ? filters : NULL, NULL, 0);

		delete[] filters;

		if (path && std::wcslen(path) > 0) {

			std::wstring* _path = new std::wstring (path);
			return _path;

		}

		#else

		std::string* _title = wstring_to_string (title);
		std::string* _filter = wstring_to_string (filter);
		std::string* _defaultPath = wstring_to_string (defaultPath);

		std::vector<std::string> filters_vec;
		if (_filter) {
			std::string line;
			std::stringstream ss(*_filter);
			while(std::getline(ss, line, ',')) {
				line.insert (0, "*.");
				filters_vec.push_back(line);
			}
		}

		const int numFilters = _filter ? filters_vec.size() : 1;
		const char **filters = new const char*[numFilters];
		if (_filter && numFilters > 0) {
			for (int index = 0; index < numFilters; index++) {
				filters[index] = const_cast<char*>(filters_vec[index].c_str());
			}
		} else {
			filters[0] = NULL;
		}

		const char* path = tinyfd_openFileDialog (_title ? _title->c_str () : NULL, _defaultPath ? _defaultPath->c_str () : NULL, _filter ? numFilters : 0, _filter ? filters : NULL, NULL, 0);

		delete[] filters;

		if (_title) delete _title;
		if (_filter) delete _filter;
		if (_defaultPath) delete _defaultPath;

		if (path && std::strlen(path) > 0) {

			std::string _path = std::string (path);
			std::wstring* __path = new std::wstring (_path.begin (), _path.end ());
			return __path;

		}

		#endif

		return 0;

#endif

	}


	void FileDialog::OpenFiles (std::vector<std::wstring*>* files, std::wstring* title, std::wstring* filter, std::wstring* defaultPath) {

#if defined(LIME_SDL3) && !defined(HX_MACOS)

		SDLFileDialogState state;

		init_state (&state, filter);

		show_file_dialog (SDL_FILEDIALOG_OPENFILE, &state, title, defaultPath, true);

		if (state.error) {

			return;

		}

		for (std::size_t i = 0; i < state.files.size (); i++) {

			files->push_back (utf8_to_wstring (state.files[i].c_str ()));

		}

#else


		std::wstring* __paths = 0;

		#ifdef HX_WINDOWS

		std::vector<std::wstring> filters_vec;
		if (filter) {
			std::wstring temp (L"*.");
			std::wstring line;
			std::wstringstream ss(*filter);
			while(std::getline(ss, line, L',')) {
				filters_vec.push_back(temp + line);
			}
		}

		const int numFilters = filter ? filters_vec.size() : 1;
		const wchar_t **filters = new const wchar_t*[numFilters];
		if (filter && numFilters > 0) {
			for (int index = 0; index < numFilters; index++) {
				filters[index] = const_cast<wchar_t*>(filters_vec[index].c_str());
			}
		} else {
			filters[0] = NULL;
		}

		const wchar_t* paths = tinyfd_openFileDialogW (title ? title->c_str () : 0, defaultPath ? defaultPath->c_str () : 0, filter ? numFilters : 0, filter ? filters : NULL, NULL, 1);

		delete[] filters;

		if (paths) {

			__paths = new std::wstring (paths);

		}

		#else

		std::string* _title = wstring_to_string (title);
		std::string* _filter = wstring_to_string (filter);
		std::string* _defaultPath = wstring_to_string (defaultPath);

		std::vector<std::string> filters_vec;
		if (_filter) {
			std::string line;
			std::stringstream ss(*_filter);
			while(std::getline(ss, line, ',')) {
				line.insert (0, "*.");
				filters_vec.push_back(line);
			}
		}

		const int numFilters = _filter ? filters_vec.size() : 1;
		const char **filters = new const char*[numFilters];
		if (_filter && numFilters > 0) {
			for (int index = 0; index < numFilters; index++) {
				filters[index] = const_cast<char*>(filters_vec[index].c_str());
			}
		} else {
			filters[0] = NULL;
		}

		const char* paths = tinyfd_openFileDialog (_title ? _title->c_str () : NULL, _defaultPath ? _defaultPath->c_str () : NULL, _filter ? numFilters : 0, _filter ? filters : NULL, NULL, 1);

		delete[] filters;

		if (_title) delete _title;
		if (_filter) delete _filter;
		if (_defaultPath) delete _defaultPath;

		if (paths) {

			std::string _paths = std::string (paths);
			__paths = new std::wstring (_paths.begin (), _paths.end ());

		}

		#endif

		if (__paths) {

			std::wstring sep = L"|";

			std::size_t start = 0, end = 0;

			while ((end = __paths->find (sep, start)) != std::wstring::npos) {

				files->push_back (new std::wstring (__paths->substr (start, end - start).c_str ()));
				start = end + 1;

			}

			files->push_back (new std::wstring (__paths->substr (start).c_str ()));

		}

#endif

	}


	std::wstring* FileDialog::SaveFile (std::wstring* title, std::wstring* filter, std::wstring* defaultPath) {

#if defined(LIME_SDL3) && !defined(HX_MACOS)

		SDLFileDialogState state;

		init_state (&state, filter);

		show_file_dialog (SDL_FILEDIALOG_SAVEFILE, &state, title, defaultPath, false);

		if (state.error || state.files.size () == 0) {

			return 0;

		}

		return utf8_to_wstring (state.files[0].c_str ());

#else


		#ifdef HX_WINDOWS

		std::wstring temp (L"*.");
		const wchar_t* filters[] = { filter ? (temp + *filter).c_str () : NULL };

		const wchar_t* path = tinyfd_saveFileDialogW (title ? title->c_str () : 0, defaultPath ? defaultPath->c_str () : 0, filter ? 1 : 0, filter ? filters : NULL, NULL);

		if (path && std::wcslen(path) > 0) {

			std::wstring* _path = new std::wstring (path);
			return _path;

		}

		#else

		std::string* _title = wstring_to_string (title);
		std::string* _filter = wstring_to_string (filter);
		std::string* _defaultPath = wstring_to_string (defaultPath);

		const char* filters[] = { NULL };

		if (_filter) {

			_filter->insert (0, "*.");
			filters[0] = _filter->c_str ();

		}

		const char* path = tinyfd_saveFileDialog (_title ? _title->c_str () : NULL, _defaultPath ? _defaultPath->c_str () : NULL, _filter ? 1 : 0, _filter ? filters : NULL, NULL);

		if (_title) delete _title;
		if (_filter) delete _filter;
		if (_defaultPath) delete _defaultPath;

		if (path && std::strlen(path) > 0) {

			std::string _path = std::string (path);
			std::wstring* __path = new std::wstring (_path.begin (), _path.end ());
			return __path;

		}

		#endif

		return 0;

#endif

	}


}
