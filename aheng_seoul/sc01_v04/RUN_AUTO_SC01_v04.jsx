/*  AHENG SEOUL | SC01 v04 | automation entry (osascript)
    osascript -e 'tell application "Adobe After Effects 2026" to DoScriptFile "/ABS/PATH/RUN_AUTO_SC01_v04.jsx"'
    - builds into a FRESH project (the open project is closed WITHOUT saving)
    - overwrites AHENG_SC01_v04_EDITABLE.aep next to this file, then aerender reads that file
    - no dialogs; the log is BUILD_LOG_SC01_v04_latest.txt next to this file
    Needs: Preferences > Scripting & Expressions > Allow Scripts to Write Files and Access Network
*/
$.global.AHENG_AUTO = true;
$.global.AHENG_OUT = File($.fileName).parent.fsName + "/AHENG_SC01_v04_EDITABLE.aep";
$.evalFile(File($.fileName).parent.fsName + "/BUILD_SC01_v04.jsx");
