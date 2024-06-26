#pragma once

#include  <Windows.h>

struct Scoped_Handle
{
	HANDLE handle;

	Scoped_Handle() :
		handle(INVALID_HANDLE_VALUE) {}
	Scoped_Handle(HANDLE handle) :
		handle(handle) {}
	Scoped_Handle(Scoped_Handle&& other) :
		handle(other.handle) {
		other.handle = NULL;
	}
	~Scoped_Handle() { if (handle != NULL && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }

	operator HANDLE() const { return handle; }

	void operator=(HANDLE p)
	{
		if (p == handle)
			return;
		if (handle != NULL && handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		handle = p;
	}

	HANDLE* operator&() { return  &handle; }
	const HANDLE* operator&()  const { return  &handle; }
};
