// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Util/Process.h>
#include <AnKi/Util/Array.h>
#include <ThirdParty/Reproc/reproc/include/reproc/reproc.h>

namespace anki {

Process::~Process()
{
	destroy();
}

void Process::destroy()
{
	if(m_handle)
	{
		ProcessStatus status;
		const Error err = getStatus(status);
		(void)err;
		if(status == ProcessStatus::RUNNING)
		{
			ANKI_UTIL_LOGE("Process is still running. Forgot to wait for it");
		}

		m_handle = reproc_destroy(m_handle);
	}
}

Error Process::start(CString executable, ConstWeakArray<CString> arguments, ConstWeakArray<CString> environment)
{
	ANKI_ASSERT(m_handle == nullptr && "Already started");

	// Set args and env
	Array<const Char*, 64> args;
	Array<const Char*, 32> env;

	args[0] = executable.cstr();
	for(U32 i = 0; i < arguments.getSize(); ++i)
	{
		args[i + 1] = arguments[i].cstr();
		ANKI_ASSERT(args[i + 1]);
	}
	args[arguments.getSize() + 1] = nullptr;

	for(U32 i = 0; i < environment.getSize(); ++i)
	{
		env[i] = environment[i].cstr();
		ANKI_ASSERT(env[i]);
	}
	env[environment.getSize()] = nullptr;

	// Start process
	m_handle = reproc_new();

	reproc_options options = {};

	options.env.behavior = REPROC_ENV_EXTEND;
	if(environment.getSize())
	{
		options.env.extra = &env[0];
	}
	options.nonblocking = true;

	options.redirect.err.type = REPROC_REDIRECT_PIPE;

	I32 ret = reproc_start(m_handle, &args[0], options);
	if(ret < 0)
	{
		ANKI_UTIL_LOGE("reproc_start() failed: %s", reproc_strerror(ret));
		m_handle = reproc_destroy(m_handle);
		return Error::USER_DATA;
	}

	ret = reproc_close(m_handle, REPROC_STREAM_IN);
	if(ret < 0)
	{
		ANKI_UTIL_LOGE("reproc_close() failed: %s. Ignoring", reproc_strerror(ret));
		m_handle = reproc_destroy(m_handle);
		return Error::USER_DATA;
	}

	return Error::NONE;
}

Error Process::wait(Second timeout, ProcessStatus* pStatus, I32* pExitCode)
{
	ANKI_ASSERT(m_handle);
	ProcessStatus status;
	I32 exitCode;

	// Compute timeout in ms
	I32 rtimeout;
	if(timeout < 0.0)
	{
		rtimeout = REPROC_INFINITE;
	}
	else
	{
		// Cap the timeout to 1h to avoid overflows when converting to ms
		if(timeout > 1.0_hour)
		{
			ANKI_UTIL_LOGW("Timeout unreasonably high (%f sec). Will cap it to 1h", timeout);
			timeout = 1.0_hour;
		}

		rtimeout = I32(timeout * 1000.0);
	}

	// Wait
	const I32 ret = reproc_wait(m_handle, rtimeout);
	if(ret == REPROC_ETIMEDOUT)
	{
		status = ProcessStatus::RUNNING;
		exitCode = 0;
	}
	else
	{
		status = ProcessStatus::NOT_RUNNING;
		exitCode = ret;
	}

	if(pStatus)
	{
		*pStatus = status;
	}

	if(pExitCode)
	{
		*pExitCode = exitCode;
	}

	ANKI_ASSERT(!(status == ProcessStatus::RUNNING && timeout < 0.0));
	return Error::NONE;
}

Error Process::getStatus(ProcessStatus& status)
{
	ANKI_ASSERT(m_handle);
	ANKI_CHECK(wait(0.0, &status, nullptr));
	return Error::NONE;
}

Error Process::kill(ProcessKillSignal k)
{
	ANKI_ASSERT(m_handle);

	I32 ret;
	CString funcName;
	if(k == ProcessKillSignal::NORMAL)
	{
		ret = reproc_terminate(m_handle);
		funcName = "reproc_terminate";
	}
	else
	{
		ANKI_ASSERT(k == ProcessKillSignal::FORCE);
		ret = reproc_kill(m_handle);
		funcName = "reproc_kill";
	}

	if(ret < 0)
	{
		ANKI_UTIL_LOGE("%s() failed: %s", funcName.cstr(), reproc_strerror(ret));
		return Error::FUNCTION_FAILED;
	}

	return Error::NONE;
}

Error Process::readFromStdout(StringAuto& text)
{
	return readCommon(REPROC_STREAM_OUT, text);
}

Error Process::readFromStderr(StringAuto& text)
{
	return readCommon(REPROC_STREAM_ERR, text);
}

Error Process::readCommon(I32 reprocStream, StringAuto& text)
{
	ANKI_ASSERT(m_handle);

	// Limit the iterations in case the process writes to FD constantly
	U32 maxIterations = 16;
	while(maxIterations--)
	{
		Array<Char, 256> buff;

		const I32 ret =
			reproc_read(m_handle, REPROC_STREAM(reprocStream), reinterpret_cast<U8*>(&buff[0]), buff.getSize() - 1);

		if(ret == 0 || ret == REPROC_EPIPE || ret == REPROC_EWOULDBLOCK)
		{
			// No data or all data have bee read or something that I don't get
			break;
		}

		if(ret < 0)
		{
			ANKI_UTIL_LOGE("reproc_read() failed: %s", reproc_strerror(ret));
			return Error::FUNCTION_FAILED;
		}

		buff[ret] = '\0';
		text.append(&buff[0]);
	}

	return Error::NONE;
}

} // end namespace anki