import lldb

def check_crash(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    process = target.process
    
    if process.GetState() == lldb.eStateExited:
        print("Program completed normally with exit code:", process.GetExitStatus())
    else:
        print("Program crashed or stopped abnormally")
        debugger.HandleCommand('thread backtrace all')
        debugger.HandleCommand('frame variable')
        debugger.HandleCommand('process status')

def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f crash_handler.check_crash check_crash')