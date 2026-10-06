package lime.system;

import lime.app.Application;
import lime.app.Event;
import lime.utils.Log;
#if sys
#if haxe4
import sys.thread.Deque;
import sys.thread.Thread;
#elseif cpp
import cpp.vm.Deque;
import cpp.vm.Thread;
#elseif neko
import neko.vm.Deque;
import neko.vm.Thread;
#end
#end
#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
class BackgroundWorker
{
	private static var MESSAGE_COMPLETE = "__COMPLETE__";
	private static var MESSAGE_ERROR = "__ERROR__";

	public var canceled(default, null):Bool;
	public var completed(default, null):Bool;
	public var doWork = new Event<Dynamic->Void>();
	public var onComplete = new Event<Dynamic->Void>();
	public var onError = new Event<Dynamic->Void>();
	public var onProgress = new Event<Dynamic->Void>();

	@:noCompletion private var __runMessage:Dynamic;
	#if (cpp || neko)
	@:noCompletion private var __messageQueue:Deque<Dynamic>;
	@:noCompletion private var __workerThread:Thread;
	#end

	public function new() {}

	public function cancel():Void
	{
		canceled = true;

		#if (cpp || neko)
		__workerThread = null;
		#end
	}

	public function run(message:Dynamic = null):Void
	{
		canceled = false;
		completed = false;
		__runMessage = message;

		#if (cpp || neko)
		__messageQueue = new Deque<Dynamic>();
		__workerThread = Thread.create(__doWork);

		// TODO: Better way to do this

		if (Application.current != null)
		{
			Application.current.onUpdate.add(__update);
		}
		#else
		__doWork();
		#end
	}

	public function sendComplete(message:Dynamic = null):Void
	{
		completed = true;

		#if (cpp || neko)
		__messageQueue.add(MESSAGE_COMPLETE);
		__messageQueue.add(message);
		#else
		if (!canceled)
		{
			canceled = true;
			onComplete.dispatch(message);
		}
		#end
	}

	public function sendError(message:Dynamic = null):Void
	{
		#if (cpp || neko)
		__messageQueue.add(MESSAGE_ERROR);
		__messageQueue.add(message);
		#else
		if (!canceled)
		{
			canceled = true;
			onError.dispatch(message);
		}
		#end
	}

	public function sendProgress(message:Dynamic = null):Void
	{
		#if (cpp || neko)
		__messageQueue.add(message);
		#else
		if (!canceled)
		{
			onProgress.dispatch(message);
		}
		#end
	}

	@:noCompletion private function __doWork():Void
	{
		doWork.dispatch(__runMessage);

		// #if (cpp || neko)
		//
		// __messageQueue.add (MESSAGE_COMPLETE);
		//
		// #else
		//
		// if (!canceled) {
		//
		// canceled = true;
		// onComplete.dispatch (null);
		//
		// }
		//
		// #end
	}

	@:noCompletion private function __update(deltaTime:Int):Void
	{
		#if (cpp || neko)
		var message = __messageQueue.pop(false);

		if (message != null)
		{
			// SeiunEngine fork ---------------------------------------------------
			// 这里跑在 lime 主循环（Application.onUpdate）里：onError/onComplete 的监听方
			// （openfl FileReference 的保存回调会在其中执行 File.saveBytes 等可抛异常的
			// 操作）一旦抛出 Haxe 异常，会穿过 ndll 的所有分发帧、找不到任何 Haxe
			// try/catch，最终以未捕获 C++ 异常（0xE06D7363）终止整个进程。
			// 因此在 dispatch 处就地兜底：吞掉、记录、并保证 canceled 语义不变
			// （仍然移除监听、只派发一次），让游戏继续运行而不是崩溃。
			if (message == MESSAGE_ERROR)
			{
				Application.current.onUpdate.remove(__update);

				if (!canceled)
				{
					canceled = true;

					try
					{
						onError.dispatch(__messageQueue.pop(false));
					}
					catch (e:Dynamic)
					{
						Log.error('BackgroundWorker onError handler failed: ${Std.string(e)}');
					}
				}
			}
			else if (message == MESSAGE_COMPLETE)
			{
				Application.current.onUpdate.remove(__update);

				if (!canceled)
				{
					canceled = true;

					try
					{
						onComplete.dispatch(__messageQueue.pop(false));
					}
					catch (e:Dynamic)
					{
						Log.error('BackgroundWorker onComplete handler failed: ${Std.string(e)}');
					}
				}
			}
			else
			{
				if (!canceled)
				{
					try
					{
						onProgress.dispatch(message);
					}
					catch (e:Dynamic)
					{
						Log.error('BackgroundWorker onProgress handler failed: ${Std.string(e)}');
					}
				}
			}
		}
		#end
	}
}
