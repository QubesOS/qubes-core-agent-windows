#pragma once
enum {
	PROGRESS_FLAG_NORMAL,
	PROGRESS_FLAG_INIT,
	PROGRESS_FLAG_DONE,
	PROGRESS_FLAG_ERROR
};

void do_notify_progress(long long written, int flag);
