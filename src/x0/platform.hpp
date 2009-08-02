#ifndef sw_x0_platform_hpp
#define sw_x0_platform_hpp (1)

// platforms
#if defined(_WIN32) || defined(__WIN32__)
#	define X0_OS_WIN32 1
//#	define _WIN32_WINNT 0x0510
#else
#	define X0_OS_UNIX 1
#	if defined(__CYGWIN__)
#		define X0_OS_WIN32 1
#	elif defined(__APPLE__)
#		define X0_OS_DARWIN 1 /* MacOS/X 10 */
#	endif
#endif

// api decl tools
#if defined(__GNUC__)
#	define X0_NO_EXPORT __attribute__((visibility("hidden")))
#	define X0_EXPORT __attribute__((visibility("default")))
#	define X0_IMPORT /*!*/
#	define X0_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#	define X0_NO_RETURN __attribute__((no_return))
#	define X0_DEPRECATED __attribute__((__deprecated__))
#	define X0_PURE __attribute__((pure))
#elif defined(__MINGW32__)
#	define X0_NO_EXPORT /*!*/
#	define X0_EXPORT __declspec(export)
#	define X0_IMPORT __declspec(import)
#	define X0_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#	define X0_NO_RETURN __attribute__((no_return))
#	define X0_DEPRECATED __attribute__((__deprecated__))
#	define X0_PURE __attribute__((pure))
#elif defined(__MSVC__)
#	define X0_NO_EXPORT /*!*/
#	define X0_EXPORT __declspec(export)
#	define X0_IMPORT __declspec(import)
#	define X0_WARN_UNUSED_RESULT /*!*/
#	define X0_NO_RETURN /*!*/
#	define X0_DEPRECATED /*!*/
#	define X0_PURE /*!*/
#else
#	warning Unknown platform
#	define X0_NO_EXPORT /*!*/
#	define X0_EXPORT /*!*/
#	define X0_IMPORT /*!*/
#	define X0_WARN_UNUSED_RESULT /*!*/
#	define X0_NO_RETURN /*!*/
#	define X0_DEPRECATED /*!*/
#	define X0_PURE /*!*/
#endif

#endif
