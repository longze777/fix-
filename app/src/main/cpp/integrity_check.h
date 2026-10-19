#pragma once

// Memory-vs-disk integrity self-check for one library.
//
// The detection this project cares about reads a system library's executable
// mapping back and diffs it against the on-disk file - the same technique that
// pinpointed the old Dobby inline hooks. After the GOT/vtable rewrite nothing
// in libaaudio.so or libOpenSLES.so .text should differ; this turns that
// property into a log line the manager's virtual-camera page can show.
//
// Line format (tag CS-Integrity):
//   <lib> region=<start>-<end> file=<path> diff_bytes=<n> verdict=PRISTINE|MODIFIED
void integrity_report(const char* lib_name);
