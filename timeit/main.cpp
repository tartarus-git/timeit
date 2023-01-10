#define WIN32_LEAN_AND_MEAN
#include <Windows.h>					// Needed for starting child processes.

#include <csignal>						// Needed for signal handling.

#include <cstdlib>						// Needed for _ui64toa_s (safe conversion function from unsigned int to ascii string) and exit function.

#include <chrono>						// Needed for timing the child process.

#include <cstdio>						// Needed for sprintf_s function.
#include <io.h>							// Needed for _isatty function and _write function.

#include <cstring>						// Needed for strcmp function and strlen function and memcpy function.
#include <string>						// Needed for the one single spot in which we use std::string.

#include <utility>						// Needed for std::pair.

//#define STDIN_FILENO 0
#define STDOUT_FILENO 1			// the only reason we have this is so we can output help text, that's it, the child process takes over for the rest of the IO
#define STDERR_FILENO 2			// for outputting the timing information, we output through stderr so that this still works even in pipelines

#define static_strlen(x) (sizeof(x) / sizeof(char) - 1) 

// ANSI escape code helpers.
#define ANSI_ESC_CODE_PREFIX "\033["
#define ANSI_ESC_CODE_SUFFIX "m"

// NOTE: const char[] and const char* are both stored in .rodata (at least usually, C++ standard leaves it open to the implementor). That means this help text is in .rodata (usually).
// NOTE: You can store strings in .data, which means they will be changeable from runtime. To do that, use char[].
// SIDE-NOTE: One would think that you could use char* as well as char[] for .data strings. That is false. char* will still point to .rodata memory, making changing the string that resides there UB.
// REASON: C did it this way, so C++ did it this way as well.
// BUT: That has been changed, I don't know when, but it's gone now. You have to use const char* for string literals now in C++. You can't use char*, which makes this whole thing much easier to understand.
// IMPORTANT: Note that you can still cast any const char* ptr to a char* ptr, and in that case you still have to remember that changing the string through the new pointer is very very UB.
const char helpText[] = "timeit runs the specified program with the specified arguments and prints the elapsed time until program completion to stderr.\n" \
							"Standard input/output/error all flow through the encapsulating timeit process to and from the target program, allowing \n" \
							"the construct to be used seamlessly within piped chains of programs.\n" \
							"\n" \
							"usage: timeit [--expand-args || --error-color <auto|on|off> ||\n" \
							"           --unit <nanoseconds | microseconds | milliseconds | seconds | minutes | hours> || --accuracy <double | int>]\n" \
							"           <program> [arguments]...\n" \
							"       timeit --help           -->            shows help text\n" \
							"\n" \
							"arguments:\n" \
								"\t--expand-args                 -->                  expand elements of [arguments]... that contain spaces into\n" \
								"\t                                                   multiple arguments (default behaviour is to leave them as single arguments)\n" \
								"\t--error-color <auto|on|off>   -->                  force a specific coloring behaviour for error messages \n" \
								"\t                                                   (which are always printed to stderr) (default behaviour is auto)\n" \
								"\t--unit <(see above)>          -->                  the unit to output the elapsed time in (default is seconds)\n" \
								"\t--accuracy <double|int>       -->                  specify whether elapsed time is outputted as a decimal (double)\n" \
								"\t                                                   or as a round number (int) (default is double)\n" \
								"\t<program>                     -->                  the program which is to be timed\n" \
								"\t[arguments]...                -->                  the arguments to pass to the target program\n" \
							"\n" \
							"NOTE: It is possible to specify a flag more than once. If this is the case, only the last occurrence will influence program behaviour.\n" \
							"\n" \
							"NOTE: On successful execution of timeit, the exit code is the exit code of the timed program. On any error, the exit code is EXIT_FAILURE.\n" \
							"      Unless of course you use \"--help\", in which case the exit code is EXIT_SUCCESS.\n" \
							"\n" \
							"NOTE: The text contained within the elements of [arguments]... is always taken literally. Quotation marks are passed\n" \
							"      as characters to the target program and DO NOT cause any grouping of arguments. All other special characters are also taken literally.\n" \
							"      Even with --expand-args specified, the arguments are only split on the spaces, the quotes are still taken as characters\n" \
							"      and don't cause any grouping.\n";

// Flag to keep track of whether we should color errors or not.
bool isErrorColored;

// Text coloring, this is only used for error messages in the case of this program.
namespace color {
	const char red[] = ANSI_ESC_CODE_PREFIX "31" ANSI_ESC_CODE_SUFFIX;
	const char reset[] = ANSI_ESC_CODE_PREFIX "0" ANSI_ESC_CODE_SUFFIX;
}

// Some of the command-line flags that one can set. The rest don't require global variables, so they're not in here.
namespace flags {
	bool expandArgs = false;
	uint8_t timeUnit = 3;
	bool timeAccuracy = false;
}

// SIDE-NOTE: It is implementation defined whether global variables that are dynamically initialized (their value isn't compile-time calculated and stored
// in .data, it needs to be calculated at runtime) are lazy initialized or whether they are initialized before main(). Don't rely on one of those behaviours.

constexpr char error_preamble[] = "timeit: ERROR: ";

namespace strlens {
	constexpr size_t error = static_strlen(error_preamble);
	constexpr size_t red = static_strlen(color::red);
	constexpr size_t reset = static_strlen(color::reset);
}

template <size_t N>
void reportError(const char (&msg)[N]) noexcept {	// NOTE: Technically, it would be more efficient to store completed, colored and uncolored versions of the full error texts as const chars, and that is totally possible with cool preprocessor and template tricks, but it isn't useful or necessary. It's actually harmful honestly.
	if (isErrorColored) {							// NOTE: Having the error strings split up into coloring, timeit: ERROR: tag, and message like this, allows the final ELF to store less .rodata in total. The opposite choice, making the error messages faster, is useless, who cares if the error messages are a little tiny bit faster at expense of memory.
		char buffer[strlens::red + strlens::error + N - 1 + strlens::reset + 1];

		// Copy all the necessary data to the buffer. This would be easier and more efficient if C++ let us do >>>> color::red "ERROR: " <<<< like it lets us do for string literals.
		// It doesn't though, which isn't so bad to be honest. I mean as stated above, doing this at compile-time would yield larger file sizes anyway, so I'm going to let it slide this time.
		// NOTE: Even if the char arrays are constexpr, it still doesn't, simply because the language spec says so. I'm sure the designers could implement that behavior if they wanted to.
		std::memcpy(buffer, color::red, strlens::red * sizeof(char));
		std::memcpy(buffer + strlens::red, error_preamble, strlens::error * sizeof(char));
		std::memcpy(buffer + strlens::red + strlens::error, msg, (N - 1) * sizeof(char));
		std::memcpy(buffer + strlens::red + strlens::error + N - 1, color::reset, strlens::reset * sizeof(char));

		buffer[strlens::red + strlens::error + N - 1 + strlens::reset] = '\n';

		_write(STDERR_FILENO, buffer, sizeof(buffer));
		// NOTE: No error handling because if outputting errors fails, how are we going to output this new error? We can't.
		// We just try our best to get the main error out before the program terminates.

		return;
	}

	char buffer[strlens::error + N - 1 + 1];

	std::memcpy(buffer, error_preamble, strlens::error * sizeof(char));
	std::memcpy(buffer + strlens::error, msg, (N - 1) * sizeof(char));

	buffer[strlens::error + N - 1] = '\n';

	// NOTE: Intellisense is complaining about this line, but PVS-Studio can't find anything. I can't see anything wrong either, so I'm leaving it like this.
	_write(STDERR_FILENO, buffer, sizeof(buffer));
}

void showHelp() noexcept {
	// NOTE: It's ok to use unbuffered IO here because we always exit after writing this, no point in buffering.
	if (_write(STDOUT_FILENO, helpText, static_strlen(helpText) * sizeof(char)) == -1) {
		reportError("failed to write help text to stdout");
		std::exit(EXIT_FAILURE);
	}
}

// This variable keeps track of the error coloring that the user requested from the command-line.
bool forcedErrorColoring;
// NOTE: GUARANTEE: If something goes wrong while parsing the cmdline args and an error message is necessary, the error message will always be printed
// with the default coloring (based on TTY/piped mode).

// Parse flag arguments. Only handles word flags (those with -- in front), since we don't have any single letter flags (those with - in front).
unsigned int parseFlags(int argc, const char* const * argv) noexcept {
// NOTE: If you write --error-color, --unit, etc... twice, the value supplied to the rightmost instance will be the value that is used. Does not throw an error.
	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		if (arg[0] == '-') {
			if (arg[1] == '-') {
				const char* flagTextStart = arg + 2;
				if (*flagTextStart == '\0') { continue; }

				// TODO: Doing this many strcmp's is not the most efficient. Maybe in the future, you can use your meta_string_matcher project to get a nice DFA in here that will efficiently parse through these options.

				if (!std::strcmp(flagTextStart, "expand-args")) { flags::expandArgs = true; continue; }

				if (!std::strcmp(flagTextStart, "error-color")) {
					i++;
					if (i == argc) { reportError("the --error-color flag was not supplied with a value"); std::exit(EXIT_FAILURE); }

					if (!std::strcmp(argv[i], "on")) { forcedErrorColoring = true; continue; }
					if (!std::strcmp(argv[i], "off")) { forcedErrorColoring = false; continue; }
					if (!std::strcmp(argv[i], "auto")) { forcedErrorColoring = isErrorColored; continue; }
					reportError("invalid value for --error-color flag"); std::exit(EXIT_FAILURE);
				}

				if (!std::strcmp(flagTextStart, "unit")) {
					i++;
					if (i == argc) { reportError("the --unit flag was not supplied with a value"); std::exit(EXIT_FAILURE); }

					if (!std::strcmp(argv[i], "nanoseconds")) { flags::timeUnit = 0; continue; }
					if (!std::strcmp(argv[i], "microseconds")) { flags::timeUnit = 1; continue; }
					if (!std::strcmp(argv[i], "milliseconds")) { flags::timeUnit = 2; continue; }
					if (!std::strcmp(argv[i], "seconds")) { flags::timeUnit = 3; continue; }
					if (!std::strcmp(argv[i], "minutes")) { flags::timeUnit = 4; continue; }
					if (!std::strcmp(argv[i], "hours")) { flags::timeUnit = 5; continue; }
					reportError("invalid value for --unit flag"); std::exit(EXIT_FAILURE);
				}

				if (!std::strcmp(flagTextStart, "accuracy")) {
					i++;
					if (i == argc) { reportError("the --accuracy flag was not supplied with a value"); std::exit(EXIT_FAILURE); }

					// NOTE: This might seem unnecessary, but (as stated above already) we need it in case the --accuracy flag
					// is used twice and has already changed the value.
					if (!std::strcmp(argv[i], "double")) { flags::timeAccuracy = false; continue; }
					if (!std::strcmp(argv[i], "int")) { flags::timeAccuracy = true; continue; }
					reportError("invalid value for --accuracy flag"); std::exit(EXIT_FAILURE);
				}

				if (!std::strcmp(flagTextStart, "help")) { showHelp(); std::exit(EXIT_SUCCESS); }

				// NOTE: Usually, I would report an error here, but since there are no valid flags with single "-", and since the following error message
				// is very applicable to this case as well, we can just fall through to the following error handling code and everything is a-okay.
			}
			reportError("one or more flag arguments are invalid"); std::exit(EXIT_FAILURE);
		}
		return i;							// Return index of first arg that isn't flag arg. Helps calling code parse args.
	}
	return argc;							// No non-flag argument was found. Return argc because it works nicely with calling code.
}

// NOTE: This is supposed to be empty. At no point in our program's execution do we ever need to react to a signal being sent,
// so the only function of this line is to stop specific signals from stopping our program and instead make them give us time.
void signalHandler(int signum) noexcept { }

// NOTE: Stop arguments with quotes in them from messing everything up. The escape syntax might look a little weird, but this is the way Windows wants it.
void prevent_injection(std::string& buffer, const char *argument) noexcept {
	const char *original_arg_ptr = argument;
	size_t backslash_count = 0;

	for (; *argument != '\0'; argument++) {
begin_for_loop_body:
		switch (*argument) {
		case '\\': backslash_count++; continue;
		default: backslash_count = 0; continue;
		case '"':
			const size_t buffer_length = buffer.length();
			const size_t copy_length = argument - original_arg_ptr;

			buffer.resize(buffer_length + copy_length + backslash_count + 2);
			// TODO: This resize function fills new data with 0. Find a way to leave it uninitialized. Make your own string class?

			char *starting_ptr = buffer.data() + buffer_length;
			std::memcpy(starting_ptr, original_arg_ptr, copy_length);

			starting_ptr += copy_length;
			char* ptr = starting_ptr;
			for (; ptr < starting_ptr + backslash_count; ptr++) { *ptr = '\\'; }

			*(ptr++) = '\\';
			*ptr = '"';

			if (*(++argument) == '\0') { return; }
			original_arg_ptr = argument;
			backslash_count = 0;
			goto begin_for_loop_body;
		}
	}

	buffer.append(original_arg_ptr, argument - original_arg_ptr);
}

// This function is responsible for actually running the program which is to be timed. The program is run as a child process of this program.
// It is given our stdout, stdin and stderr handles and it is able to freely interact with the console and the user, or with pipes, should some be set up.
std::pair<std::chrono::nanoseconds, int> runChildProcess(int argc, const char * const * argv) noexcept {
	// NOTE: I thought this was impossible, but apparently it isn't, so we have to check for it and report an error if it occurs.
	// NOTE: The reason we don't just let CreateProcessA detect this error is because it will probably just filter out the nothingness and use the first argument as the program name, which is very terrible.
	// NOTE: We don't care about the other arguments being zero because those get passed onto the child process anyway. It'll be the one to deal with any discrepencies in that regard.
	if (argv[0][0] == '\0') { reportError("target program name cannot be empty"); std::exit(EXIT_FAILURE); }

	std::string buffer;
	buffer += '\"';	// IMPORTANT NOTE: These are super necessary because you wouldn't otherwise be able to run programs that have names with spaces in them. Even worse, you could accidentally run a completely different program than the one you wanted, which is terrible behaviour. That is why we add the quotes, to prevent against those two things.
	prevent_injection(buffer, argv[0]);
	// NOTE: prevent_injection() is super important because if the user get's quotation mark characters into the program name,
	// it could cause the parser to parse the program name completely differently, which is pretty dangerous.
	// To prevent that, prevent_injection escapes all the quotation marks properly according to Window's weird escape
	// syntax.
	buffer += "\" ";
	// NOTE: The difference in behavior here is achieved by adding quotes around the arguments. When quotes are present, arguments with spaces are still
	// considered as one argument. Without quotes, the given arguments will be split up into many arguments when CreateProcessA creates the child process.
	if (flags::expandArgs) {
		for (int i = 1; i < argc; i++) {
			prevent_injection(buffer, argv[i]);
			// NOTE: You would think that allowing injection here would prove to be useful, and you would be right, BUT there is the following problem:
			// The syntax Windows uses for escapes is terrible and I don't want my program's users to have to deal with that.
			// SOLUTION: Prevent Windows escaping but allow our own custom escaping.
			// PROBLEM WITH THAT SOLUTION: That's not standard, so it's something extra that the users are going to have to remember.
			// BETTER SOLUTION: Use whatever system Linux uses, that would be acceptable.
			// TODO: Do the above solution, until then, we're just going to disallow injection here.
			buffer += ' ';
		}
	}
	else {
		for (int i = 1; i < argc; i++) {
			buffer += '\"';
			prevent_injection(buffer, argv[i]);		// NOTE: We prevent injection here because we want this to be one single argument no matter what.
			buffer += "\" ";
		}
	}

	PROCESS_INFORMATION procInfo;	// NOTE: No need to set the fields to zero, since this is only for getting data out from CreateProcessA. The OS fills these.

	// We could fill this struct with information about how exactly the target program is to be started, which permissions it has, etc...,
	// but all that is unnecessary for this application, so we just zero everything out and set the mandatory cb field.
	STARTUPINFOA startupInfo { };
	startupInfo.cb = sizeof(STARTUPINFOA);

	std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();

	// Finally actually create the child process.
	// IMPORTANT NOTE: We have to use buffer.data() instead of buffer.c_str() because data() returns a modifiable c-string, which is necessary for CreateProcessA. The deal is that you're allowed to modify everything except the trailing NUL character, doing so would be UB.
	// CreateProcessA almost definitely won't change the NUL character because why should it, and it won't change the c-string pointer because it just accepts a copy, meaning it literally can't. So the only thing it can really change is the meat of the c-string, which we don't care about.
	// Also, I think (if I'm reading them right) the docs said that CreateProcessW is the one that changes the string, not CreateProcessA, so we should be safe anyway. Strange that the argument isn't marked with const in CreateProcessA then.
	if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, false, 0, nullptr, nullptr, &startupInfo, &procInfo)) {
		reportError("couldn't start target program");	// NOTE: Sadly, the win32 docs don't really specify the possible error codes for CreateProcessA, so I don't know which cases to check for here. This generic error message will have to do.
		std::exit(EXIT_FAILURE);
	}

	// Close the hThread handle as soon as possible since we never actually use it for anything.
	// NOTE: Don't worry about something depending on it. All handles in Windows are ref-counted AFAIK, so as long as the child process is
	// active and is using this handle, which it almost definitely is, it won't actually be closed until our child exits.
	if (!CloseHandle(procInfo.hThread)) {
		reportError("failed to close child process main thread handle");
		CloseHandle(procInfo.hProcess);
		std::exit(EXIT_FAILURE);
	}

	// Wait for the child process to exit. NOTE: If SIGINT or SIGBREAK is sent, we assume that the child process will handle it and exit, so that we can exit as well. If that doesn't happen and the child hangs, we hang with it so the user knows that something is hanging.
	// If we didn't hang when the child hung, we could exit and the child would still exist, possibly without the user ever knowing. Since the child still has access to the console, maybe the console wouldn't continue until the child also exits, but we do it like this just in case (and also because it is easier).
	// Plus, if we hang and the OS breaks us off, it's not really that bad. We don't have any unfreed resources (except hProcess, which isn't a big deal) at this point.
	if (WaitForSingleObject(procInfo.hProcess, INFINITE) == WAIT_FAILED) {
		reportError("failed to wait for child process to finish execution");
		CloseHandle(procInfo.hProcess);
		std::exit(EXIT_FAILURE);
	}

	std::pair<std::chrono::nanoseconds, DWORD> result_pair;
	result_pair.first = std::chrono::high_resolution_clock::now() - startTime;

	if (!GetExitCodeProcess(procInfo.hProcess, &result_pair.second)) {
		reportError("failed to get child process exit code");
		CloseHandle(procInfo.hProcess);
		std::exit(EXIT_FAILURE);
	}

	if (!CloseHandle(procInfo.hProcess)) {
		reportError("failed to close child process handle");
		std::exit(EXIT_FAILURE);
	}

	return result_pair;
}

// Number to string conversion functions.

#define ELAPSED_TIME_STRING_SIZE 128

// NOTE: type (&name)[size] is the syntax for an array reference, which is super useful in some cases. Why? Because it enforces the size of the array at compile-time. So you can't pass anything into the function except an array of the correct size.
// NOTE: Compiler has no way of checking this properly if you use pointers, which means those aren't allowed either, which is also useful in some cases.
void intToString(char (&output)[ELAPSED_TIME_STRING_SIZE], uint64_t value) noexcept {
	// TODO: You should make your own algorithm for doing this. These can get sort of complicated if you tune them to your hardware, but it's a good excersize, you should do it.
	if (_ui64toa_s(value, output, ELAPSED_TIME_STRING_SIZE, 10) != 0) {					// NOTE: An unsigned int64 can be 20 digits long at maximum, which is why we technically only have to write to the first 21 bytes of the output array (cuz NUL character).
		reportError("failed to convert elapsed time integer to string");				// NOTE: The output array is larger because it needs to work with doubleToString as well. We use ELAPSED_TIME_STRING here even though we don't need the full size just in case,
		std::exit(EXIT_FAILURE);														// and because any slow-down that it might cause isn't noticable and doesn't effect the functionality of the program in any way whatsoever.
	}
}

void doubleToString(char (&output)[ELAPSED_TIME_STRING_SIZE], double value) noexcept {
	if (sprintf_s(output, ELAPSED_TIME_STRING_SIZE, "%f", value) == -1) {				// NOTE: The buffer should be large enough for this never to overflow. Even if it isn't, sprintf_s will return -1 before it causes any sort of overflow, since it's a safe function.
		reportError("failed to convert elapsed time double to string");
		std::exit(EXIT_FAILURE);
	}
}

DWORD manageArgs(int argc, const char* const * argv) noexcept {
	unsigned int targetProgramIndex = parseFlags(argc, argv);
	unsigned int targetProgramArgCount = argc - targetProgramIndex;
	switch (targetProgramArgCount) {
	case 0:
		reportError("too few arguments"); std::exit(EXIT_FAILURE);
	default:
		isErrorColored = forcedErrorColoring;	// If everything went great with parsing the cmdline args, finally set output coloring to what the user wants it to be. It is necessary to do this here because of the garantee that we wrote above.

		const std::pair<std::chrono::nanoseconds, DWORD> elapsedTime_exit_code_pair = runChildProcess(targetProgramArgCount, argv + targetProgramIndex);
		const std::chrono::nanoseconds& elapsedTime = elapsedTime_exit_code_pair.first;

		char elapsedTimeString[ELAPSED_TIME_STRING_SIZE];
		switch (flags::timeUnit) {
		case 0:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::nano>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(elapsedTime).count()); break;
		case 1:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::micro>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(elapsedTime).count()); break;
		case 2:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::milli>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsedTime).count()); break;
		case 3:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double>>(elapsedTime).count()); break;
		case 4:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<60, 1>>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::ratio<60, 1>>>(elapsedTime).count()); break;
		case 5:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<3600, 1>>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::ratio<3600, 1>>>(elapsedTime).count()); break;
		}

		const size_t elapsedTimeStringLength = std::strlen(elapsedTimeString);
		elapsedTimeString[elapsedTimeStringLength] = '\n';	// Replace NUL termination character with newline. We don't need the NUL character anyway.
		if (_write(STDERR_FILENO, elapsedTimeString, (elapsedTimeStringLength + 1) * sizeof(char)) == -1) {
			reportError("failed to write elapsed time to stderr");
			std::exit(EXIT_FAILURE);
		}

		return elapsedTime_exit_code_pair.second;
	}
}

int main(int argc, char* const * argv) {
	// These two lines make it so that the program ignores both SIGINT and SIGBREAK signals as best as possible.
	// SIGBREAK can only be ignored for 5 seconds, but that's actually good.
	signal(SIGINT, signalHandler);
	signal(SIGBREAK, signalHandler);
	// NOTE: noexcept function pointers implicitly cast to non-noexcept function pointers, which is super nice.
	// The only reason this doesn't work in std::thread is because it's got templates and stuff which hinder this casting.
	// SOLUTION: It's possible to make std::thread better I think, the stdlib just needs to be improved. TODO: Do that.

	if (_isatty(STDERR_FILENO)) {	// If stderr is connected to a tty, we need to enable ANSI escape codes so that we can output error messages in color. We also set a pair of flags so the rest of the program knows about the tty-status.
		HANDLE stdErrHandle = GetStdHandle(STD_ERROR_HANDLE);
		if (!stdErrHandle || stdErrHandle == INVALID_HANDLE_VALUE) { goto ANSISetupFailure; }
		DWORD mode;
		if (!GetConsoleMode(stdErrHandle, &mode)) { goto ANSISetupFailure; }
		if (!SetConsoleMode(stdErrHandle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) { goto ANSISetupFailure; }

		isErrorColored = true;
		forcedErrorColoring = true;
	}
	else { ANSISetupFailure: isErrorColored = false; forcedErrorColoring = false; }

	return manageArgs(argc, argv);	// NOTE: Windows is giving us no choice but to cast DWORD (unsigned long) to int. It's perfectly defined behavior, even though there could potentially be some overflowing. I guess Windows is okay with that.
}