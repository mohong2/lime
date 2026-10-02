package lime.tools;

/**
 * Bilingual (English / Simplified Chinese) message table for the lime command
 * line tools. Same rules as the hxcpp tool's table:
 *   - language: SEIUN_LANG / LIME_LANG -> LANG / LANGUAGE -> console code page
 *     (936 / 950 / 54936 => Chinese Windows) -> English
 *   - the CLI runs on Neko and writes raw UTF-8; the console code page is read by
 *     running `chcp` once. When it is not 65001 we fall back to ASCII + English
 *     (Neko cannot call Win32 directly: neko.Lib.load only loads *.ndll).
 *   - SEIUN_UTF8=1 / SEIUN_ASCII=1 force the decision.
 */
class I18n
{
	public static var lang(default, null):String = "en";
	public static var unicodeOk(default, null):Bool = true;
	public static var codePage(default, null):Int = 0;

	static var tables:Map<String, Map<String, String>> = null;

	public static function init():Void
	{
		if (tables != null)
			return;

		codePage = detectCodePage();

		var asciiEnv = norm(Sys.getEnv("SEIUN_ASCII"));
		var utf8Env = norm(Sys.getEnv("SEIUN_UTF8"));
		if (isOn(asciiEnv))
			unicodeOk = false;
		else if (isOn(utf8Env))
			unicodeOk = true;
		else
			unicodeOk = (codePage <= 0) || (codePage == 65001);

		lang = pickLang();
		if (!unicodeOk)
			lang = "en";

		tables = buildTables();
	}

	public static function t(key:String, ?args:Dynamic):String
	{
		init();
		var table = tables.exists(lang) ? tables.get(lang) : tables.get("en");
		var text = table.exists(key) ? table.get(key) : null;
		if (text == null)
			text = tables.get("en").exists(key) ? tables.get("en").get(key) : key;
		if (args != null)
			text = subst(text, args);
		return text;
	}

	/** Display width, counting CJK code points as two cells (byte based, target safe). */
	public static function width(text:String):Int
	{
		var bytes = haxe.io.Bytes.ofString(text);
		var w = 0;
		var i = 0;
		while (i < bytes.length)
		{
			var b = bytes.get(i);
			if (b < 0x80) { w += 1; i += 1; }
			else if (b < 0xE0) { w += 1; i += 2; }
			else if (b < 0xF0) { w += 2; i += 3; }
			else { w += 2; i += 4; }
		}
		return w;
	}

	public static function pad(text:String, target:Int):String
	{
		var missing = target - width(text);
		return missing > 0 ? text + StringTools.lpad("", " ", missing) : text;
	}

	static function subst(text:String, args:Dynamic):String
	{
		var re = ~/\{([a-zA-Z_][a-zA-Z0-9_]*)\}/g;
		return re.map(text, function(r:EReg) {
			var value:Dynamic = Reflect.field(args, r.matched(1));
			return value == null ? "" : Std.string(value);
		});
	}

	static function pickLang():String
	{
		for (name in ["SEIUN_LANG", "LIME_LANG", "LANG", "LANGUAGE"])
		{
			var value = norm(Sys.getEnv(name));
			if (value == null || value == "")
				continue;
			if (StringTools.startsWith(value, "zh") || value.indexOf("chinese") >= 0 || value.indexOf("hans") >= 0)
				return "zh";
			if (StringTools.startsWith(value, "en"))
				return "en";
		}

		// A console whose code page is 65001 (the "UTF-8 worldwide" option) says
		// nothing about the UI language, so ask Windows directly. Neko cannot call
		// Win32 (neko.Lib.load only loads *.ndll), so this is a cheap registry read.
		var locale = systemLocale();
		if (locale != null)
		{
			if (StringTools.startsWith(locale, "zh") || locale.indexOf("chinese") >= 0 || locale.indexOf("hans") >= 0)
				return "zh";
			if (StringTools.startsWith(locale, "en"))
				return "en";
		}

		if (codePage == 936 || codePage == 950 || codePage == 54936)
			return "zh";
		return "en";
	}

	static function systemLocale():String
	{
		if (Sys.systemName() != "Windows")
			return null;
		try
		{
			var process = new sys.io.Process("reg", ["query", "HKCU\\Control Panel\\International", "/v", "LocaleName"]);
			var output = "";
			try { output = process.stdout.readAll().toString(); } catch (e:Dynamic) {}
			process.close();
			var re = ~/LocaleName\s+REG_SZ\s+(\S+)/;
			if (re.match(output))
				return norm(re.matched(1));
		}
		catch (e:Dynamic) {}
		return null;
	}

	static function detectCodePage():Int
	{
		if (Sys.systemName() != "Windows")
			return 0;
		try
		{
			var process = new sys.io.Process("chcp", []);
			var output = "";
			try { output = process.stdout.readAll().toString(); } catch (e:Dynamic) {}
			process.close();
			var re = ~/([0-9]{3,5})/;
			if (re.match(output))
				return Std.parseInt(re.matched(1));
		}
		catch (e:Dynamic) {}
		return 0;
	}

	static function isOn(value:String):Bool
	{
		return value == "1" || value == "true" || value == "yes" || value == "on";
	}

	static function norm(value:String):String
	{
		return value == null ? null : value.toLowerCase();
	}

	static function buildTables():Map<String, Map<String, String>>
	{
		var en = new Map<String, String>();
		en.set("report.title", "Build summary");
		en.set("report.target", "target");
		en.set("report.commands", "commands");
		en.set("report.output", "output");
		en.set("report.binary", "binary");
		en.set("report.elapsed", "elapsed");
		en.set("report.next", "next");
		en.set("report.run", "haxelib run lime run {target}");
		en.set("progress.bar", "[{bar}] {pct}% {done}/{total} ETA {eta}");
		en.set("progress.done", "{label}: {total} items in {elapsed}");

		var zh = new Map<String, String>();
		zh.set("report.title", "构建摘要");
		zh.set("report.target", "目标平台");
		zh.set("report.commands", "执行命令");
		zh.set("report.output", "输出目录");
		zh.set("report.binary", "可执行文件");
		zh.set("report.elapsed", "耗时");
		zh.set("report.next", "下一步");
		zh.set("report.run", "haxelib run lime run {target}");
		zh.set("progress.bar", "[{bar}] {pct}% {done}/{total} 剩余 {eta}");
		zh.set("progress.done", "{label}: 共 {total} 项, 用时 {elapsed}");

		var tables = new Map<String, Map<String, String>>();
		tables.set("en", en);
		tables.set("zh", zh);
		return tables;
	}
}
