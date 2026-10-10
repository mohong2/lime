package;

import hxp.*;
import sys.io.File;
import sys.io.Process;
import sys.FileSystem;

class RunScript
{
	private static function rebuildTools(rebuildBinaries = true):Void
	{
		var limeDirectory = Haxelib.getPath(new Haxelib("lime"), true);
		var toolsDirectory = Path.combine(limeDirectory, "tools");

		if (!FileSystem.exists(toolsDirectory))
		{
			toolsDirectory = Path.combine(limeDirectory, "../tools");
		}

		/*var extendedToolsDirectory = Haxelib.getPath (new Haxelib ("lime-extended"), false);

			if (extendedToolsDirectory != null && extendedToolsDirectory != "") {

				var buildScript = File.getContent (Path.combine (extendedToolsDirectory, "tools.hxml"));
				buildScript = StringTools.replace (buildScript, "\r\n", "\n");
				buildScript = StringTools.replace (buildScript, "\n", " ");

				System.runCommand (toolsDirectory, "haxe", buildScript.split (" "));

		} else {*/

		System.runCommand(toolsDirectory, "haxe", ["tools.hxml"]);

		// }

		if (!rebuildBinaries) return;

		// NOTE: the host lime.ndll is intentionally NOT rebuilt here.
		//
		// The tools are compiled with optional CFFI support, so tools.n runs
		// without a prebuilt lime.ndll. Rebuilding the ndll from this bootstrap
		// path used to inject an unrequested, host-architecture build into the
		// start of every "lime <command>" invocation: on ARM64 macOS runners that
		// bootstrap build was x86_64 (it carries none of the user's defines, so it
		// cannot honor -arm64 or the requested target), it wasted minutes per job,
		// and a failure there aborted the command before the requested build ever
		// started. The ndll for the requested target is built by the explicit
		// "lime rebuild <target>" step instead.
	}

	public static function runCommand(path:String, command:String, args:Array<String>, throwErrors:Bool = true):Int
	{
		var oldPath:String = "";

		if (path != null && path != "")
		{
			oldPath = Sys.getCwd();

			try
			{
				Sys.setCwd(path);
			}
			catch (e:Dynamic)
			{
				Log.error("Cannot set current working directory to \"" + path + "\"");
			}
		}

		var result:Dynamic = Sys.command(command, args);

		if (oldPath != "")
		{
			Sys.setCwd(oldPath);
		}

		if (throwErrors && result != 0)
		{
			Sys.exit(1);
		}

		return result;
	}

	public static function main()
	{
		var args = Sys.args();

		if (args.length > 2 && args[0] == "rebuild" && args[1] == "tools")
		{
			var lastArgument = new Path(args[args.length - 1]).toString();
			var cacheDirectory = Sys.getCwd();

			if (((StringTools.endsWith(lastArgument, "/") && lastArgument != "/") || StringTools.endsWith(lastArgument, "\\"))
				&& !StringTools.endsWith(lastArgument, ":\\"))
			{
				lastArgument = lastArgument.substr(0, lastArgument.length - 1);
			}

			if (FileSystem.exists(lastArgument) && FileSystem.isDirectory(lastArgument))
			{
				Sys.setCwd(lastArgument);
			}

			Haxelib.workingDirectory = Sys.getCwd();
			var rebuildBinaries = true;

			for (arg in args)
			{
				var equals = arg.indexOf("=");

				if (equals > -1 && StringTools.startsWith(arg, "--"))
				{
					var argValue = arg.substr(equals + 1);
					var field = arg.substr(2, equals - 2);

					if (StringTools.startsWith(field, "haxelib-"))
					{
						var name = field.substr(8);
						Haxelib.pathOverrides.set(name, Path.tryFullPath(argValue));
					}
				}
				else if (StringTools.startsWith(arg, "-"))
				{
					switch (arg)
					{
						case "-v", "-verbose":
							Log.verbose = true;

						case "-nocolor":
							Log.enableColor = false;

						case "-nocffi":
							rebuildBinaries = false;

						default:
					}
				}
			}

			rebuildTools(rebuildBinaries);

			if (args.indexOf("-openfl") > -1)
			{
				Sys.setCwd(cacheDirectory);
			}
			else
			{
				Sys.exit(0);
			}
		}

		if (!FileSystem.exists("tools/tools.n") || args.indexOf("-rebuild") > -1)
		{
			rebuildTools();
		}

		var args = ["tools/tools.n"].concat(args);
		Sys.exit(runCommand("", "neko", args));
	}
}
