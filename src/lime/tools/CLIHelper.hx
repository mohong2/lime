package lime.tools;

import haxe.io.Eof;
import hxp.*;

class CLIHelper
{
	public static function ask(question:String, options:Array<String> = null):Answer
	{
		if (options == null)
		{
			options = ["y", "n", "a"];
		}

		while (true)
		{
			Log.print(Log.accentColor + question + "\x1b[0m \x1b[3;37m[" + options.join("/") + "]\x1b[0m ? ");

			try
			{
				switch (readLine())
				{
					case "n":
						return NO;
					case "y":
						return YES;
					case "a":
						return ALWAYS;
					case _ => x if (options.indexOf(x) > -1):
						return CUSTOM(x);
				}
			}
			catch (e:Dynamic)
			{
				Sys.exit(0);
			}
		}

		return null;
	}

	public static inline function getChar():Int
	{
		return Sys.getChar(false);
	}

	public static function param(name:String, passwd:Bool = false):String
	{
		Log.print(name + ": ");

		if (passwd)
		{
			var s = new StringBuf();
			var c;
			while ((c = getChar()) != 13)
				s.addChar(c);

			Log.print("");
			Log.println("");

			return s.toString();
		}

		try
		{
			return readLine();
		}
		catch (e:Eof)
		{
			return "";
		}
	}

	static var drawn:Bool = false;
	static var lastLen:Int = 0;
	static var progressStart:Float = 0;
	static var lastPercent:Int = -1;
	static inline var BAR_WIDTH:Int = 24;

	/**
	 * In-place progress bar:
	 *   [############------------]  47% 1281/2726 ETA 3m41s
	 * Modes via SEIUN_PROGRESS / LIME_PROGRESS: bar (default), line (one line per
	 * 10%, used in CI), off. Call progressDone() when the loop ends.
	 */
	public static function progress(prefix:String, now:Int, total:Int):Void
	{
		if (total <= 0)
			return;

		var percent = Math.floor(now / total * 100);

		if (percent > 100)
			percent = 100;

		if (progressStart == 0)
			progressStart = Sys.time();

		if (now >= total)
		{
			progressDone();
			return;
		}

		var mode = progressMode();

		if (mode == "off")
			return;

		if (mode == "line")
		{
			if (percent >= lastPercent + 10)
			{
				lastPercent = percent;
				Log.println("  " + renderBar(percent, prefix, now, total));
			}
			return;
		}

		var text = renderBar(percent, prefix, now, total);
		var textWidth = I18n.width(text);
		var padding = lastLen > textWidth ? lastLen - textWidth : 0;
		Log.print("\r" + text + (padding > 0 ? StringTools.lpad("", " ", padding) : ""));
		lastLen = textWidth;
		drawn = true;
	}

	/** Erase the in-place bar line; safe to call even when nothing was drawn. */
	public static function progressDone():Void
	{
		if (drawn)
		{
			Log.print("\r" + (lastLen > 0 ? StringTools.lpad("", " ", lastLen) : "") + "\r");
			drawn = false;
			lastLen = 0;
		}

		lastPercent = -1;
		progressStart = 0;
	}

	public static function fmtDuration(seconds:Float):String
	{
		var s = Math.round(seconds);

		if (s < 60)
			return s + "s";

		var m = Math.floor(s / 60);
		var r = s - m * 60;

		if (m < 60)
			return m + "m" + (r < 10 ? "0" : "") + r + "s";

		var h = Math.floor(m / 60);
		var rest = m - h * 60;

		return h + "h" + (rest < 10 ? "0" : "") + rest + "m";
	}

	static function renderBar(percent:Int, prefix:String, now:Int, total:Int):String
	{
		I18n.init();

		var unicode = I18n.unicodeOk;
		var filled = Math.floor(BAR_WIDTH * percent / 100);
		var bar = "";

		for (i in 0...BAR_WIDTH)
			bar += (i < filled) ? (unicode ? "\u2588" : "#") : (unicode ? "\u2591" : "-");

		var elapsed = progressStart > 0 ? Sys.time() - progressStart : 0;
		var eta = (now > 0 && total > now) ? elapsed * (total - now) / now : 0;
		var label = (prefix != null && prefix != "") ? prefix + " " : "";

		return label + I18n.t("progress.bar", {bar: bar, pct: percent, done: now, total: total, eta: fmtDuration(eta)});
	}

	static function progressMode():String
	{
		I18n.init();

		var mode = Sys.getEnv("SEIUN_PROGRESS");

		if (mode == null || mode == "")
			mode = Sys.getEnv("LIME_PROGRESS");

		if (mode != null)
		{
			mode = mode.toLowerCase();

			if (mode == "off" || mode == "0" || mode == "false" || mode == "none")
				return "off";
			if (mode == "line")
				return "line";
			if (mode == "bar")
				return "bar";
		}

		for (name in ["CI", "GITHUB_ACTIONS", "TF_BUILD", "BUILDKITE", "GITLAB_CI", "JENKINS_URL", "APPVEYOR"])
			if (Sys.getEnv(name) != null)
				return "line";

		return "bar";
	}

	public static inline function readLine():String
	{
		return Sys.stdin().readLine();
	}
}

enum Answer
{
	YES;
	NO;
	ALWAYS;
	CUSTOM(answer:String);
}
