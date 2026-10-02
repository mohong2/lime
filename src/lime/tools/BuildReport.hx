package lime.tools;

import hxp.*;
import sys.FileSystem;

/**
 * End-of-build summary card: what was built, where it landed, how long it took
 * and what to run next. Printed once per `lime build` / `rebuild` / `run`,
 * never for commands that produced nothing. Disable with LIME_NO_SUMMARY=1.
 */
class BuildReport
{
	public static function print(target:PlatformTarget, startTime:Float, executed:Array<String>):Void
	{
		if (executed == null || executed.length == 0)
			return;
		if (Sys.getEnv("LIME_NO_SUMMARY") != null)
			return;

		I18n.init();

		var project = target.project;
		var elapsed = Sys.time() - startTime;
		var rows = new Array<Array<String>>();

		rows.push([I18n.t("report.target"), Std.string(project.target).toLowerCase() + " (" + target.buildType + ")"]);
		rows.push([I18n.t("report.commands"), executed.join(", ")]);

		var outputPath = project.app.path;
		if (outputPath != null && outputPath != "")
			rows.push([I18n.t("report.output"), outputPath]);

		var binary = findBinary(project, outputPath);
		if (binary != null)
			rows.push([I18n.t("report.binary"), binary]);

		rows.push([I18n.t("report.elapsed"), CLIHelper.fmtDuration(elapsed)]);
		rows.push([I18n.t("report.next"), I18n.t("report.run", {target: Std.string(project.target).toLowerCase()})]);

		var labelWidth = 0;
		for (row in rows)
		{
			var w = I18n.width(row[0]);
			if (w > labelWidth)
				labelWidth = w;
		}

		Log.println("");
		Log.println("  " + Log.accentColor + "== " + I18n.t("report.title") + " ==" + Log.resetColor);

		for (row in rows)
		{
			Log.println("  " + Log.accentColor + I18n.pad(row[0], labelWidth) + Log.resetColor + " : " + row[1]);
		}

		Log.println("");
	}

	static function findBinary(project:HXProject, outputPath:String):String
	{
		if (outputPath == null || outputPath == "" || project.app.file == null)
			return null;

		// cpp/hl targets put the binary in <app.path>/<target>/bin, others straight
		// into <app.path>
		var target = Std.string(project.target).toLowerCase();
		var directories = [outputPath, Path.combine(Path.combine(outputPath, target), "bin"), Path.combine(outputPath, target)];

		for (directory in directories)
		{
			for (suffix in ["", ".exe", ".app", ".swf", ".js", ".html"])
			{
				var candidate = Path.combine(directory, project.app.file + suffix);

				if (FileSystem.exists(candidate) && !FileSystem.isDirectory(candidate))
				{
					var size = FileSystem.stat(candidate).size;
					var human = size >= 1048576 ? (Math.round(size / 1048576.0 * 10) / 10) + " MB" : Math.round(size / 1024.0) + " KB";
					return candidate + " (" + human + ")";
				}
			}
		}

		return null;
	}
}
