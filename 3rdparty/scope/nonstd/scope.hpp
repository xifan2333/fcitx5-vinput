//
// Copyright (c) 2020-2025 Martin Moene
//
// https://github.com/martinmoene/scope-lite
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// C++ standard libraries extensions, version 3
// https://en.cppreference.com/w/cpp/experimental/lib_extensions_3

#ifndef NONSTD_SCOPE_LITE_HPP
#define NONSTD_SCOPE_LITE_HPP

#define scope_lite_MAJOR  0
#define scope_lite_MINOR  3
#define scope_lite_PATCH  0

#define scope_lite_VERSION  scope_STRINGIFY(scope_lite_MAJOR) "." scope_STRINGIFY(scope_lite_MINOR) "." scope_STRINGIFY(scope_lite_PATCH)

#define scope_STRINGIFY(  x )  scope_STRINGIFY_( x )
#define scope_STRINGIFY_( x )  #x

// scope-lite configuration:

#define scope_SCOPE_DEFAULT  0
#define scope_SCOPE_NONSTD   1
#define scope_SCOPE_STD      2

// tweak header support:

#if defined(__has_include) && __has_include(<nonstd/scope.tweak.hpp>)
# include <nonstd/scope.tweak.hpp>
# define scope_HAVE_TWEAK_HEADER  1
#else
# define scope_HAVE_TWEAK_HEADER  0
//# pragma message("scope.hpp: Note: Tweak header not supported.")
#endif

// scope selection and configuration:

#if !defined( scope_CONFIG_SELECT_SCOPE )
# define scope_CONFIG_SELECT_SCOPE  ( scope_HAVE_STD_SCOPE ? scope_SCOPE_STD : scope_SCOPE_NONSTD )
#endif

#if !defined( scope_CONFIG_NO_EXTENSIONS )
# define scope_CONFIG_NO_EXTENSIONS  0
#endif

#if !defined( scope_CONFIG_NO_CONSTEXPR )
# define scope_CONFIG_NO_CONSTEXPR  (scope_CONFIG_NO_EXTENSIONS || !scope_CPP20_OR_GREATER)
#endif

// C++ language version detection (C++23 is speculative):
// Note: VC14.0/1900 (VS2015) lacks too much from C++14.

#ifndef   scope_CPLUSPLUS
# if defined(_MSVC_LANG ) && !defined(__clang__)
#  define scope_CPLUSPLUS  (_MSC_VER == 1900 ? 201103L : _MSVC_LANG )
# else
#  define scope_CPLUSPLUS  __cplusplus
# endif
#endif

#define scope_CPP98_OR_GREATER  ( scope_CPLUSPLUS >= 199711L )
#define scope_CPP11_OR_GREATER  ( scope_CPLUSPLUS >= 201103L )
#define scope_CPP14_OR_GREATER  ( scope_CPLUSPLUS >= 201402L )
#define scope_CPP17_OR_GREATER  ( scope_CPLUSPLUS >= 201703L )
#define scope_CPP20_OR_GREATER  ( scope_CPLUSPLUS >= 202002L )
#define scope_CPP23_OR_GREATER  ( scope_CPLUSPLUS >= 202300L )

// Use C++yy <scope> if available and requested:
// Note: __cpp_lib_experimental_scope: a value of at least 201902 indicates that the scope guard are supported

#if scope_CPP20_OR_GREATER && defined(__has_include )
# if   __has_include( <scope> )
#  define scope_HAVE_STD_SCOPE  1
#  define scope_HAVE_EXP_SCOPE  0
# elif __has_include( <experimental/scope> )
#  define scope_HAVE_STD_SCOPE  0
#  define scope_HAVE_EXP_SCOPE  1
# else
#  define scope_HAVE_STD_SCOPE  0
#  define scope_HAVE_EXP_SCOPE  0
# endif
#else
# define  scope_HAVE_STD_SCOPE  0
# define  scope_HAVE_EXP_SCOPE  0
#endif

#define  scope_USES_STD_SCOPE  ( (scope_CONFIG_SELECT_SCOPE == scope_SCOPE_STD) || ((scope_CONFIG_SELECT_SCOPE == scope_SCOPE_DEFAULT) && scope_HAVE_STD_SCOPE) )
#define  scope_USES_EXP_SCOPE  ( (scope_CONFIG_SELECT_SCOPE == scope_SCOPE_STD) || ((scope_CONFIG_SELECT_SCOPE == scope_SCOPE_DEFAULT) && scope_HAVE_EXP_SCOPE) )

#if scope_USES_STD_SCOPE

#include <scope>

namespace nonstd
{
    using std::scope_exit;
    using std::scope_fail;
    using std::scope_success;
    using std::unique_resource;

    using std::make_scope_exit;
    using std::make_scope_fail;
    using std::make_scope_success;
    using std::make_unique_resource_checked;
}

#elif scope_USES_EXP_SCOPE

#include <experimental/scope>

namespace nonstd
{
    using std::experimental::scope_exit;
    using std::experimental::scope_fail;
    using std::experimental::scope_success;
    using std::experimental::unique_resource;

    using std::experimental::make_scope_exit;
    using std::experimental::make_scope_fail;
    using std::experimental::make_scope_success;
    using std::experimental::make_unique_resource_checked;
}

#else // scope_USES_STD_SCOPE

// half-open range [lo..hi):
#define scope_BETWEEN( v, lo, hi ) ( (lo) <= (v) && (v) < (hi) )

// Compiler versions:
//
// MSVC++  6.0  _MSC_VER == 1200  scope_COMPILER_MSVC_VERSION ==  60  (Visual Studio 6.0)
// MSVC++  7.0  _MSC_VER == 1300  scope_COMPILER_MSVC_VERSION ==  70  (Visual Studio .NET 2002)
// MSVC++  7.1  _MSC_VER == 1310  scope_COMPILER_MSVC_VERSION ==  71  (Visual Studio .NET 2003)
// MSVC++  8.0  _MSC_VER == 1400  scope_COMPILER_MSVC_VERSION ==  80  (Visual Studio 2005)
// MSVC++  9.0  _MSC_VER == 1500  scope_COMPILER_MSVC_VERSION ==  90  (Visual Studio 2008)
// MSVC++ 10.0  _MSC_VER == 1600  scope_COMPILER_MSVC_VERSION == 100  (Visual Studio 2010)
// MSVC++ 11.0  _MSC_VER == 1700  scope_COMPILER_MSVC_VERSION == 110  (Visual Studio 2012)
// MSVC++ 12.0  _MSC_VER == 1800  scope_COMPILER_MSVC_VERSION == 120  (Visual Studio 2013)
// MSVC++ 14.0  _MSC_VER == 1900  scope_COMPILER_MSVC_VERSION == 140  (Visual Studio 2015)
// MSVC++ 14.1  _MSC_VER >= 1910  scope_COMPILER_MSVC_VERSION == 141  (Visual Studio 2017)
// MSVC++ 14.2  _MSC_VER >= 1920  scope_COMPILER_MSVC_VERSION == 142  (Visual Studio 2019)

#if defined(_MSC_VER ) && !defined(__clang__)
# define scope_COMPILER_MSVC_VER      (_MSC_VER )
# define scope_COMPILER_MSVC_VERSION  (_MSC_VER / 10 - 10 * ( 5 + (_MSC_VER < 1900 ) ) )
#else
# define scope_COMPILER_MSVC_VER      0
# define scope_COMPILER_MSVC_VERSION  0
#endif

// Courtesy of https://github.com/gsl-lite/gsl-lite
// AppleClang  7.0.0  __apple_build_version__ ==  7000172  scope_COMPILER_APPLECLANG_VERSION ==  700  (Xcode 7.0, 7.0.1)          (LLVM 3.7.0)
// AppleClang  7.0.0  __apple_build_version__ ==  7000176  scope_COMPILER_APPLECLANG_VERSION ==  700  (Xcode 7.1)                 (LLVM 3.7.0)
// AppleClang  7.0.2  __apple_build_version__ ==  7000181  scope_COMPILER_APPLECLANG_VERSION ==  702  (Xcode 7.2, 7.2.1)          (LLVM 3.7.0)
// AppleClang  7.3.0  __apple_build_version__ ==  7030029  scope_COMPILER_APPLECLANG_VERSION ==  730  (Xcode 7.3)                 (LLVM 3.8.0)
// AppleClang  7.3.0  __apple_build_version__ ==  7030031  scope_COMPILER_APPLECLANG_VERSION ==  730  (Xcode 7.3.1)               (LLVM 3.8.0)
// AppleClang  8.0.0  __apple_build_version__ ==  8000038  scope_COMPILER_APPLECLANG_VERSION ==  800  (Xcode 8.0)                 (LLVM 3.9.0)
// AppleClang  8.0.0  __apple_build_version__ ==  8000042  scope_COMPILER_APPLECLANG_VERSION ==  800  (Xcode 8.1, 8.2, 8.2.1)     (LLVM 3.9.0)
// AppleClang  8.1.0  __apple_build_version__ ==  8020038  scope_COMPILER_APPLECLANG_VERSION ==  810  (Xcode 8.3)                 (LLVM 3.9.0)
// AppleClang  8.1.0  __apple_build_version__ ==  8020041  scope_COMPILER_APPLECLANG_VERSION ==  810  (Xcode 8.3.1)               (LLVM 3.9.0)
// AppleClang  8.1.0  __apple_build_version__ ==  8020042  scope_COMPILER_APPLECLANG_VERSION ==  810  (Xcode 8.3.2, 8.3.3)        (LLVM 3.9.0)
// AppleClang  9.0.0  __apple_build_version__ ==  9000037  scope_COMPILER_APPLECLANG_VERSION ==  900  (Xcode 9.0)                 (LLVM 4.0.0?)
// AppleClang  9.0.0  __apple_build_version__ ==  9000038  scope_COMPILER_APPLECLANG_VERSION ==  900  (Xcode 9.1)                 (LLVM 4.0.0?)
// AppleClang  9.0.0  __apple_build_version__ ==  9000039  scope_COMPILER_APPLECLANG_VERSION ==  900  (Xcode 9.2)                 (LLVM 4.0.0?)
// AppleClang  9.1.0  __apple_build_version__ ==  9020039  scope_COMPILER_APPLECLANG_VERSION ==  910  (Xcode 9.3, 9.3.1)          (LLVM 5.0.2?)
// AppleClang  9.1.0  __apple_build_version__ ==  9020039  scope_COMPILER_APPLECLANG_VERSION ==  910  (Xcode 9.4, 9.4.1)          (LLVM 5.0.2?)
// AppleClang 10.0.0  __apple_build_version__ == 10001145  scope_COMPILER_APPLECLANG_VERSION == 1000  (Xcode 10.0, 10.1)          (LLVM 6.0.1?)
// AppleClang 10.0.1  __apple_build_version__ == 10010046  scope_COMPILER_APPLECLANG_VERSION == 1001  (Xcode 10.2, 10.2.1, 10.3)  (LLVM 7.0.0?)
// AppleClang 11.0.0  __apple_build_version__ == 11000033  scope_COMPILER_APPLECLANG_VERSION == 1100  (Xcode 11.1, 11.2, 11.3)    (LLVM 8.0.0?)

#define scope_COMPILER_VERSION( major, minor, patch )  ( 10 * ( 10 * (major) + (minor) ) + (patch) )

#if defined( __apple_build_version__ )
# define scope_COMPILER_APPLECLANG_VERSION scope_COMPILER_VERSION( __clang_major__, __clang_minor__, __clang_patchlevel__ )
# define scope_COMPILER_CLANG_VERSION 0
#elif defined( __clang__ )
# define scope_COMPILER_APPLECLANG_VERSION 0
# define scope_COMPILER_CLANG_VERSION scope_COMPILER_VERSION( __clang_major__, __clang_minor__, __clang_patchlevel__ )
#else
# define scope_COMPILER_APPLECLANG_VERSION 0
# define scope_COMPILER_CLANG_VERSION 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
# define scope_COMPILER_GNUC_VERSION  scope_COMPILER_VERSION(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__)
#else
# define scope_COMPILER_GNUC_VERSION  0
#endif

// Presence of language and library features:

#define scope_HAVE( feature )  ( scope_HAVE_##feature )

#ifdef _HAS_CPP0X
# define scope_HAS_CPP0X  _HAS_CPP0X
#else
# define scope_HAS_CPP0X  0
#endif

#define scope_CPP11_90   (scope_CPP11_OR_GREATER || scope_COMPILER_MSVC_VER >= 1500)
#define scope_CPP11_100  (scope_CPP11_OR_GREATER || scope_COMPILER_MSVC_VER >= 1600)
#define scope_CPP11_110  (scope_CPP11_OR_GREATER || scope_COMPILER_MSVC_VER >= 1700)
#define scope_CPP11_120  (scope_CPP11_OR_GREATER || scope_COMPILER_MSVC_VER >= 1800)
#define scope_CPP11_140  (scope_CPP11_OR_GREATER || scope_COMPILER_MSVC_VER >= 1900)

#define scope_CPP14_000  (scope_CPP14_OR_GREATER)

#define scope_CPP17_000  (scope_CPP17_OR_GREATER)
#define scope_CPP17_140  (scope_CPP17_OR_GREATER || scope_COMPILER_MSVC_VER >= 1900)

// Presence of C++11 language features:

#define scope_HAVE_CONSTEXPR_11           scope_CPP11_140
// #define scope_HAVE_ENUM_CLASS             scope_CPP11_110
#define scope_HAVE_IS_DEFAULT             scope_CPP11_120
#define scope_HAVE_IS_DELETE              scope_CPP11_120
#define scope_HAVE_NOEXCEPT               scope_CPP11_140
#define scope_HAVE_DEFAULT_FUNCTION_TEMPLATE_ARG  scope_CPP11_120
#define scope_HAVE_STATIC_ASSERT          scope_CPP11_90
#define scope_HAVE_TRAILING_RETURN_TYPE   scope_CPP11_120
#define scope_HAVE_VALUE_INITIALIZATION   scope_CPP11_120

// Presence of C++14 language features:

#define scope_HAVE_CONSTEXPR_14           scope_CPP14_000

// Presence of C++17 language features:

#define scope_HAVE_DEDUCTION_GUIDES       scope_CPP17_000
#define scope_HAVE_NODISCARD              scope_CPP17_000

// Presence of C++11 library features:

#define scope_HAVE_IS_TRIVIAL             scope_CPP11_110
#define scope_HAVE_IS_TRIVIALLY_COPYABLE  scope_CPP11_110 && !scope_BETWEEN(scope_COMPILER_GNUC_VERSION, 1, 500) // GCC >= 5
#define scope_HAVE_IS_CONSTRUCTIBLE       scope_CPP11_110
#define scope_HAVE_IS_COPY_CONSTRUCTIBLE  scope_CPP11_110
#define scope_HAVE_IS_MOVE_CONSTRUCTIBLE  scope_CPP11_110
#define scope_HAVE_IS_NOTHROW_CONSTRUCTIBLE scope_CPP11_110
#define scope_HAVE_IS_NOTHROW_COPY_CONSTRUCTIBLE scope_CPP11_110
#define scope_HAVE_IS_NOTHROW_MOVE_CONSTRUCTIBLE scope_CPP11_110
#define scope_HAVE_IS_COPY_ASSIGNABLE     scope_CPP11_110
#define scope_HAVE_IS_NOTHROW_ASSIGNABLE  scope_CPP11_110
#define scope_HAVE_IS_NOTHROW_MOVE_ASSIGNABLE  scope_CPP11_110

#define scope_HAVE_FUNCATIONAL            scope_CPP11_110
#define scope_HAVE_REFERENCE_WRAPPER      scope_HAVE_FUNCATIONAL

#define scope_HAVE_REMOVE_CV              scope_CPP11_90
#define scope_HAVE_REMOVE_REFERENCE       scope_CPP11_90

#define scope_HAVE_TYPE_TRAITS            scope_CPP11_110
#define scope_HAVE_TR1_TYPE_TRAITS        ((!! scope_COMPILER_GNUC_VERSION ) && scope_CPP11_OR_GREATER)

#define scope_HAVE_DECAY                  scope_CPP11_110
#define scope_HAVE_DECAY_TR1              scope_HAVE_TR1_TYPE_TRAITS

#define scope_HAVE_IS_SAME                scope_HAVE_TYPE_TRAITS
#define scope_HAVE_IS_SAME_TR1            scope_HAVE_TR1_TYPE_TRAITS

// #define scope_HAVE_CSTDINT                scope_CPP11_90

// Presence of C++14 library features:

// Presence of C++17 library features:

#define scope_HAVE_UNCAUGHT_EXCEPTIONS    scope_CPP17_140

// Presence of C++ language features:

#if scope_HAVE_CONSTEXPR_11
# define scope_constexpr constexpr
#else
# define scope_constexpr /*constexpr*/
#endif

#if scope_HAVE_CONSTEXPR_14
# define scope_constexpr14 constexpr
#else
# define scope_constexpr14 /*constexpr*/
#endif

#if !scope_CONFIG_NO_CONSTEXPR
#define scope_constexpr_ext  constexpr
#else
# define scope_constexpr_ext /*constexpr*/
#endif

#if scope_HAVE( IS_DELETE )
# define scope_is_delete = delete
# define scope_is_delete_access public
#else
# define scope_is_delete
# define scope_is_delete_access private
#endif

#if scope_HAVE_NOEXCEPT
# define scope_noexcept noexcept
# define scope_noexcept_op(expr) noexcept(expr)
#else
# define scope_noexcept /*noexcept*/
# define scope_noexcept_op(expr) /*noexcept(expr)*/
#endif

#if scope_HAVE_NODISCARD
# define scope_nodiscard [[nodiscard]]
#else
# define scope_nodiscard /*[[nodiscard]]*/
#endif

#if scope_HAVE_STATIC_ASSERT
# define scope_static_assert(expr, msg) static_assert((expr), msg)
#else
# define scope_static_assert(expr, msg) /*static_assert((expr), msg)*/
#endif

// Select C++98 version

#define scope_USE_POST_CPP98_VERSION  scope_CPP11_100

// Additional includes:

#include <exception>    // exception, terminate(), uncaught_exceptions()
#include <limits>       // std::numeric_limits<>
#include <utility>      // move(), forward<>(), swap()

#if scope_HAVE_FUNCATIONAL
# include <functional>
#endif

#if scope_HAVE_TYPE_TRAITS
# include <type_traits>
#elif scope_HAVE_TR1_TYPE_TRAITS
# include <tr1/type_traits>
#endif

// Method enabling (return type):

#if scope_HAVE( TYPE_TRAITS )
# define scope_ENABLE_IF_R_(VA, R)  typename std::enable_if< (VA), R >::type
#else
# define scope_ENABLE_IF_R_(VA, R)  R
#endif

// Method enabling (function template argument):

#if scope_HAVE( TYPE_TRAITS ) && scope_HAVE( DEFAULT_FUNCTION_TEMPLATE_ARG )
// VS 2013 seems to have trouble with SFINAE for default non-type arguments:
# if !scope_BETWEEN( scope_COMPILER_MSVC_VERSION, 1, 140 )
#  define scope_ENABLE_IF_(VA) , typename std::enable_if< ( VA ), int >::type = 0
# else
#  define scope_ENABLE_IF_(VA) , typename = typename std::enable_if< ( VA ), ::nonstd::scope::enabler >::type
# endif
#else
# define  scope_ENABLE_IF_(VA)
#endif

// Declare __cxa_get_globals() or equivalent in namespace nonstd::scope for uncaught_exceptions():

#if !scope_HAVE( UNCAUGHT_EXCEPTIONS )
# if scope_COMPILER_MSVC_VERSION                                // libstl :)
    namespace nonstd { namespace scope { extern "C" char * __cdecl _getptd(); }}
# elif scope_COMPILER_CLANG_VERSION || scope_COMPILER_GNUC_VERSION || scope_COMPILER_APPLECLANG_VERSION
# if defined(__GLIBCXX__) || defined(__GLIBCPP__)               // libstdc++: prototype from cxxabi.h
#   include  <cxxabi.h>
# elif !defined(BOOST_CORE_UNCAUGHT_EXCEPTIONS_HPP_INCLUDED_)   // libc++: prototype from Boost?
# if defined(__FreeBSD__) || defined(__OpenBSD__)
    namespace __cxxabiv1 { struct __cxa_eh_globals; extern "C" __cxa_eh_globals * __cxa_get_globals(); }
# else
    namespace __cxxabiv1 { struct __cxa_eh_globals; extern "C" __cxa_eh_globals * __cxa_get_globals() scope_noexcept; }
# endif
# endif
    namespace nonstd { namespace scope { using ::__cxxabiv1::__cxa_get_globals; }}
# endif // scope_COMPILER_MSVC_VERSION
#endif // !scope_HAVE( UNCAUGHT_EXCEPTIONS )

// Namespace nonstd:

namespace nonstd {
namespace scope {

// for scope_ENABLE_IF_():

/*enum*/ class enabler{};

// C++11 emulation:

namespace std11 {

template< class T, T v > struct integral_constant { enum { value = v }; };
template< bool B       > struct bool_constant : integral_constant<bool, B>{};

typedef bool_constant< true  > true_type;
typedef bool_constant< false > false_type;

template <class T> struct is_reference      : false_type{};
template <class T> struct is_reference<T&>  : true_type {};
#if scope_CPP11_100
template <class T> struct is_reference<T&&> : true_type {};
#endif

template< class T > struct remove_pointer                    { typedef T type; };
template< class T > struct remove_pointer<T*>                { typedef T type; };
template< class T > struct remove_pointer<T* const>          { typedef T type; };
template< class T > struct remove_pointer<T* volatile>       { typedef T type; };
template< class T > struct remove_pointer<T* const volatile> { typedef T type; };

template<bool B, class T, class F>
struct conditional { typedef T type; };

template<class T, class F>
struct conditional<false, T, F> { typedef F type; };

#if scope_HAVE( DECAY )
    using std::decay;
#elif scope_HAVE( DECAY_TR1 )
    using std::tr1::decay;
#else
    template< class T > struct decay{ typedef T type; };
#endif

#if scope_HAVE( IS_TRIVIAL )
    using std::is_trivial;
#else
    template< class T > struct is_trivial : std11::true_type{};
#endif

#if scope_HAVE( IS_TRIVIALLY_COPYABLE )
    using std::is_trivially_copyable;
#else
    template< class T > struct is_trivially_copyable : std11::true_type{};
#endif

#if scope_HAVE( IS_CONSTRUCTIBLE )
    using std::is_constructible;
#else
    template< class T > struct is_constructible : std11::true_type{};
#endif

#if scope_HAVE( IS_COPY_CONSTRUCTIBLE )
    using std::is_copy_constructible;
#else
    template< class T > struct is_copy_constructible : std11::true_type{};
#endif

#if scope_HAVE( IS_MOVE_CONSTRUCTIBLE )
    using std::is_move_constructible;
#else
    template< class T > struct is_move_constructible : std11::true_type{};
#endif

#if scope_HAVE( IS_NOTHROW_CONSTRUCTIBLE )
    using std::is_nothrow_constructible;
#else
    template< class T, class U > struct is_nothrow_constructible : std11::true_type{};
#endif

#if scope_HAVE( IS_NOTHROW_COPY_CONSTRUCTIBLE )
    using std::is_nothrow_copy_constructible;
#else
    template< class T > struct is_nothrow_copy_constructible : std11::true_type{};
#endif

#if scope_HAVE( IS_NOTHROW_MOVE_CONSTRUCTIBLE )
    using std::is_nothrow_move_constructible;
#else
    template< class T > struct is_nothrow_move_constructible : std11::true_type{};
#endif

#if scope_HAVE( IS_COPY_ASSIGNABLE )
    using std::is_copy_assignable;
#else
    template< class T > struct is_copy_assignable : std11::true_type{};
#endif

#if scope_HAVE( IS_NOTHROW_ASSIGNABLE )
    using std::is_nothrow_assignable;
#else
    template< class T, class U > struct is_nothrow_assignable : std11::true_type{};
#endif

#if scope_HAVE( IS_NOTHROW_MOVE_ASSIGNABLE )
    using std::is_nothrow_move_assignable;
#else
    template< class T > struct is_nothrow_move_assignable : std11::true_type{};
#endif

#if scope_HAVE( IS_SAME )
    using std::is_same;
#elif scope_HAVE( IS_SAME_TR1 )
    using std::tr1::is_same;
#else
    template< class T, class U > struct is_same : std11::true_type{};
#endif

#if scope_HAVE( REMOVE_CV )
    using std::remove_cv;
#else
    template< class T > struct remove_cv{ typedef T type; };
#endif

#if scope_HAVE( REMOVE_REFERENCE )
    using std::remove_reference;
#else
    template< class T > struct remove_reference{ typedef T type; };
#endif

#if scope_HAVE( REFERENCE_WRAPPER )
    using std::reference_wrapper;
#else
    template< class T > struct reference_wrapper{ typedef T type; };
#endif

} // namespace std11

// C++14 emulation:

namespace std14 {

#if scope_CPP11_100
#if scope_HAVE( DEFAULT_FUNCTION_TEMPLATE_ARG )
template< class T, class U = T >
#else
template< class T, class U /*= T*/ >
#endif
scope_constexpr14 T exchange( T & obj, U && new_value )
{
    T old_value = std::move( obj );
    obj = std::forward<U>( new_value );
    return old_value;
}
#else
// C++98 version?
#endif

} // namespace std14

// C++17 emulation (uncaught_exceptions):

namespace std17 {

template< typename T >
inline int to_int( T x ) scope_noexcept
{
    return static_cast<int>( x );
}

#if scope_HAVE( UNCAUGHT_EXCEPTIONS )

inline int uncaught_exceptions() scope_noexcept
{
    return to_int( std::uncaught_exceptions() );
}

#elif scope_COMPILER_MSVC_VERSION

inline int uncaught_exceptions() scope_noexcept
{
    return to_int( *reinterpret_cast<const unsigned*>(_getptd() + (sizeof(void*) == 8 ? 0x100 : 0x90) ) );
}

#elif scope_COMPILER_CLANG_VERSION || scope_COMPILER_GNUC_VERSION || scope_COMPILER_APPLECLANG_VERSION

inline int uncaught_exceptions() scope_noexcept
{
    return to_int( *reinterpret_cast<const unsigned*>(
        reinterpret_cast<const unsigned char*>(__cxa_get_globals()) + sizeof(void*) ) );
}

#endif // scope_HAVE( UNCAUGHT_EXCEPTIONS )

} // namespace std17

// C++20 emulation:

namespace std20 {

template< class T >
struct remove_cvref
{
    typedef typename std11::remove_cv<typename std11::remove_reference<T>::type>::type type;
};

template< class T, class U >
struct same_as : std11::integral_constant<bool, std11::is_same<T,U>::value && std11::is_same<U,T>::value> {};

template< class T >
struct type_identity { typedef T type; };

} // namespace std20

namespace detail {

#if scope_CONFIG_NO_CONSTEXPR

using std17::uncaught_exceptions;

#else
constexpr int uncaught_exceptions() noexcept
{
    if ( std::is_constant_evaluated() )
    {
        return 0;
    }
    else
    {
        return std17::uncaught_exceptions();
    }
}
#endif

} // namespace detail

//
// For reference:
//

#if 0

template< class EF>
class scope_exit;

template< class EF>
class scope_fail;

template< class EF>
class scope_success;

template<class R,class D>
class unique_resource;

// special factory function:

template<class R,class D, class S=R>
unique_resource<decay_t<R>, decay_t<D>>
make_unique_resource_checked(R&& r, S const& invalid, D&& d)
noexcept(is_nothrow_constructible_v<decay_t<R>, R> && is_nothrow_constructible_v<decay_t<D>, D>);

// optional factory functions (should at least be present for LFTS3):

template< class EF>
scope_exit<decay_t<EF>>
make_scope_exit(EF&& exit_function) ;

template< class EF>
scope_fail<decay_t<EF>>
make_scope_fail(EF&& exit_function) ;

template< class EF>
scope_success<decay_t<EF>>
make_scope_success(EF&& exit_function) ;

#endif // reference

#if scope_USE_POST_CPP98_VERSION

//
// Post-C++98 version:
//

template< typename T >
T && conditional_forward( T && t, std11::true_type )
{
    return std::forward<T>( t );
}

template< typename T >
T const & conditional_forward( T && t, std11::false_type )
{
    return t;
}

template< typename T >
T && conditional_move( T && t, std11::true_type )
{
    return std::move( t );
}

template< typename T >
T const & conditional_move( T && t, std11::false_type )
{
    return t;
}

// template< typename FE, typename Fn >
// struct to_argument_type<EF,Fn>
// {
// };

// scope_exit:

template< class EF >
class scope_exit
{
public:
    template< class Fn
        scope_ENABLE_IF_((
            !std11::is_same<typename std20::remove_cvref<Fn>::type, scope_exit>::value
            && std11::is_constructible<EF, Fn>::value
        ))
    >
    scope_constexpr_ext explicit scope_exit( Fn&& fn )
    scope_noexcept_op
    ((
        std11::is_nothrow_constructible<EF, Fn>::value
        || std11::is_nothrow_constructible<EF, Fn&>::value
    ))
        : exit_function(
//            to_argument_type<EF,Fn>( std::forward<Fn>(fn) ) )
            conditional_forward<Fn>( std::forward<Fn>(fn)
                , std11::bool_constant< std11::is_nothrow_constructible<EF, Fn>::value >() ) )
        , execute_on_destruction( true )
    {}

    scope_constexpr_ext scope_exit( scope_exit && other )
    scope_noexcept_op
    ((
        std11::is_nothrow_move_constructible<EF>::value
        || std11::is_nothrow_copy_constructible<EF>::value
    ))
        : exit_function( std::forward<EF>( other.exit_function ) )
        , execute_on_destruction( other.execute_on_destruction )
    {
        other.release();
    }

    scope_constexpr_ext ~scope_exit() scope_noexcept
    {
        if ( execute_on_destruction )
            exit_function();
    }

    scope_constexpr_ext void release() scope_noexcept
    {
        execute_on_destruction = false;
    }

scope_is_delete_access:
    scope_constexpr_ext scope_exit( scope_exit const & ) scope_is_delete;

    scope_constexpr_ext scope_exit & operator=( scope_exit const & ) scope_is_delete;
    scope_constexpr_ext scope_exit & operator=( scope_exit &&      ) scope_is_delete;

private:
    EF exit_function;
    bool execute_on_destruction; // { true };
};

// scope_fail:

template< class EF >
class scope_fail
{
public:
    template< class Fn
        scope_ENABLE_IF_((
            !std11::is_same<typename std20::remove_cvref<Fn>::type, scope_fail>::value
            && std11::is_constructible<EF, Fn>::value
        ))
    >
    scope_constexpr_ext explicit scope_fail( Fn&& fn )
    scope_noexcept_op
    ((
        std11::is_nothrow_constructible<EF, Fn>::value
        || std11::is_nothrow_constructible<EF, Fn&>::value
    ))
        : exit_function(
            conditional_forward<Fn>( std::forward<Fn>(fn)
            , std11::bool_constant< std11::is_nothrow_constructible<EF, Fn>::value >() ) )
        , uncaught_on_creation( detail::uncaught_exceptions() )
    {}

    scope_constexpr_ext scope_fail( scope_fail && other )
    scope_noexcept_op
    ((
        std11::is_nothrow_move_constructible<EF>::value
        || std11::is_nothrow_copy_constructible<EF>::value
    ))
        : exit_function( std::forward<EF>( other.exit_function ) )
        , uncaught_on_creation( other.uncaught_on_creation )
    {
        other.release();
    }

    scope_constexpr_ext ~scope_fail() scope_noexcept
    {
        if ( uncaught_on_creation < detail::uncaught_exceptions() )
            exit_function();
    }

    scope_constexpr_ext void release() scope_noexcept
    {
        uncaught_on_creation = std::numeric_limits<int>::max();
    }

scope_is_delete_access:
    scope_constexpr_ext scope_fail( scope_fail const & ) scope_is_delete;

    scope_constexpr_ext scope_fail & operator=( scope_fail const & ) scope_is_delete;
    scope_constexpr_ext scope_fail & operator=( scope_fail &&      ) scope_is_delete;

private:
    EF exit_function;
    int uncaught_on_creation; // { detail::uncaught_exceptions() };
};

// scope_success:

template< class EF >
class scope_success
{
public:
    template< class Fn
        scope_ENABLE_IF_((
            !std11::is_same<typename std20::remove_cvref<Fn>::type, scope_success>::value
            && std11::is_constructible<EF, Fn>::value
        ))
    >
    scope_constexpr_ext explicit scope_success( Fn&& fn )
    scope_noexcept_op
    ((
        std11::is_nothrow_constructible<EF, Fn>::value
        || std11::is_nothrow_constructible<EF, Fn&>::value
    ))
        : exit_function(
            conditional_forward<Fn>( std::forward<Fn>(fn)
            , std11::bool_constant< std11::is_nothrow_constructible<EF, Fn>::value >() ) )
        , uncaught_on_creation( detail::uncaught_exceptions() )
    {}

    scope_constexpr_ext scope_success( scope_success && other )
    scope_noexcept_op
    ((
        std11::is_nothrow_move_constructible<EF>::value
        || std11::is_nothrow_copy_constructible<EF>::value
    ))
        : exit_function( std::forward<EF>( other.exit_function ) )
        , uncaught_on_creation( other.uncaught_on_creation )
    {
        other.release();
    }

    scope_constexpr_ext ~scope_success()
#if !scope_BETWEEN(scope_COMPILER_GNUC_VERSION, 1, 900) // GCC >= 9, issue #12
        scope_noexcept_op( scope_noexcept_op(this->exit_function()) )
#endif
    {
        if ( uncaught_on_creation >= detail::uncaught_exceptions() )
            exit_function();
    }

    scope_constexpr_ext void release() scope_noexcept
    {
        uncaught_on_creation = -1;
    }

scope_is_delete_access:
    scope_constexpr_ext scope_success( scope_success const & ) scope_is_delete;

    scope_constexpr_ext scope_success & operator=( scope_success const & ) scope_is_delete;
    scope_constexpr_ext scope_success & operator=( scope_success &&      ) scope_is_delete;

private:
    EF exit_function;
    int uncaught_on_creation; // { detail::uncaught_exceptions() };
};

#if scope_HAVE( DEDUCTION_GUIDES )
template< class EF > scope_exit(EF) -> scope_exit<EF>;
template< class EF > scope_fail(EF) -> scope_fail<EF>;
template< class EF > scope_success(EF) -> scope_success<EF>;
#endif

// optional factory functions (should at least be present for LFTS3):

template< class EF >
scope_constexpr_ext
scope_exit<typename std11::decay<EF>::type>
make_scope_exit( EF && exit_function )
{
    return scope_exit<typename std11::decay<EF>::type>( std::forward<EF>( exit_function ) );
}

template< class EF >
scope_constexpr_ext
scope_fail<typename std11::decay<EF>::type>
make_scope_fail( EF && exit_function )
{
    return scope_fail<typename std11::decay<EF>::type>( std::forward<EF>( exit_function ) );
}

template< class EF >
scope_constexpr_ext
scope_success<typename std11::decay<EF>::type>
make_scope_success( EF && exit_function )
{
    return scope_success<typename std11::decay<EF>::type>( std::forward<EF>( exit_function ) );
}

// unique_resource:

template< class R, class D >
class unique_resource
{
private:
    scope_static_assert(
        (  std11::is_move_constructible<R>::value && std11::is_nothrow_move_constructible<R>::value )
        || std11::is_copy_constructible<R>::value
        , "resource must be nothrow_move_constructible or copy_constructible"
    );

    scope_static_assert(
          (std11::is_move_constructible<D>::value && std11::is_nothrow_move_constructible<D>::value )
        || std11::is_copy_constructible<D>::value
        , "deleter must be nothrow_move_constructible or copy_constructible"
    );

    typedef typename std11::conditional<
        std11::is_reference<R>::value
        , typename std11::reference_wrapper< typename std11::remove_reference<R>::type >::type
        , R
    >::type R1;

public:
    // This overload only participates in overload resolution if:
    // - std::is_default_constructible_v<R>
    // - && std::is_default_constructible_v<D>

    unique_resource()
#if scope_HAVE( VALUE_INITIALIZATION )
        : resource{}
        , deleter{}
        , execute_on_reset{ false }
#else
        : resource()
        , deleter()
        , execute_on_reset( false )
#endif
    {}

    // construction: note extra execute default parameter

    template< class RR, class DD
#if scope_BETWEEN( scope_COMPILER_MSVC_VERSION, 120, 130 )
        scope_ENABLE_IF_(( true
        //  &&  std11::is_constructible<R1, RR>::value
            &&  std11::is_constructible<D , DD>::value
        //  && (std11::is_nothrow_constructible<R1, RR>::value || std11::is_constructible<R1, RR&>::value )
            &&  std11::is_nothrow_constructible<D, DD>::value  || std11::is_constructible<D, DD&>::value
        ))
#else
        scope_ENABLE_IF_(( true
            &&  std11::is_constructible<R1, RR>::value
            &&  std11::is_constructible<D , DD>::value
            && (std11::is_nothrow_constructible<R1, RR>::value || std11::is_constructible<R1, RR&>::value )
            &&  std11::is_nothrow_constructible<D, DD>::value  || std11::is_constructible<D, DD&>::value
        ))
#endif
    >
    unique_resource( RR && r, DD && d, bool execute = true )
        scope_noexcept_op((
            ( std11::is_nothrow_constructible<R1, RR>::value || std11::is_nothrow_constructible<R1, RR&>::value )
            && ( std11::is_nothrow_constructible<D, DD>::value || std11::is_nothrow_constructible<D, DD&>::value )
        ))
        : resource( conditional_forward<RR>( std::forward<RR>(r)
            , std11::bool_constant< std11::is_nothrow_constructible<R1, RR>::value >() ) )
        , deleter( ( conditional_forward<DD>( std::forward<DD>(d)
            , std11::bool_constant< std11::is_nothrow_constructible<D, DD>::value >() ) ) )
        , execute_on_reset( execute )
    {}

    // Move constructor.
    //
    // The stored resource handle is initialized from the one of other, using std::move if
    // std::is_nothrow_move_constructible_v<RS> is true.
    //
    // If initialization of the stored resource handle throws an exception, other is not modified.
    //
    // Then, the deleter is initialized with the one of other, using std::move if
    // std::is_nothrow_move_constructible_v<D> is true.
    //
    // If initialization of the deleter throws an exception and std::is_nothrow_move_constructible_v<RS> is true and
    // other owns the resource, calls the deleter of other with res_ to dispose the resource, then calls other.release().
    //
    // After construction, the constructed unique_resource owns its resource if and only if other owned the resource before
    // the construction, and other is set to not own the resource.

    unique_resource( unique_resource && other )
        scope_noexcept_op(
            std11::is_nothrow_move_constructible<R1>::value && std11::is_nothrow_move_constructible<D>::value
        )
    try
        : resource( conditional_move( std::move(other.resource), typename std11::bool_constant< std11::is_nothrow_move_assignable<R>::value >() ) )
        , deleter(  conditional_move( std::move(other.deleter ), typename std11::bool_constant< std11::is_nothrow_move_constructible<D>::value >() ) )
        , execute_on_reset( std14::exchange( other.execute_on_reset, false ) )
    {}
    catch(...)
    {
        if ( other.execute_on_reset && std11::is_nothrow_move_constructible<R>::value )
        {
            other.get_deleter()( this->get() );
            other.release();
        }
    }

    ~unique_resource()
    {
        reset();
    }

private:
    // assign_rd( r, is_nothrow_move_assignable_v<R>, is_nothrow_move_assignable_v<D> ):

    void assign_rd( unique_resource && other, std11::true_type, std11::true_type )
    {
        resource = std::move( other.resource );
        deleter  = std::move( other.deleter );
    }

    void assign_rd( unique_resource && other, std11::true_type, std11::false_type )
    {
        resource = std::move( other.resource );
        deleter  = other.deleter;
    }

    void assign_rd( unique_resource && other, std11::false_type, std11::true_type )
    {
        deleter  = std::move( other.deleter );
        resource = other.resource;
    }

    void assign_rd( unique_resource && other, std11::false_type, std11::false_type )
    {
        resource = other.resource;
        deleter  = other.deleter;
    }

public:
    unique_resource & operator=( unique_resource && other )
        scope_noexcept_op(
            std11::is_nothrow_move_assignable<R1>::value && std11::is_nothrow_move_assignable<D>::value
        )
    {
        scope_static_assert(
            std11::is_nothrow_move_assignable<R>::value || std11::is_copy_assignable<R>::value
            , "The resource must be nothrow-move assignable, or copy assignable"
        );

        scope_static_assert(
            std11::is_nothrow_move_assignable<D>::value || std11::is_copy_assignable<D>::value
            , "The deleter must be nothrow-move assignable, or copy assignable");

        if ( &other != this )
        {
            reset();
            assign_rd(
                std::move( other )
                , typename std11::bool_constant< std11::is_nothrow_move_assignable<R>::value >()
                , typename std11::bool_constant< std11::is_nothrow_move_assignable<D>::value >()
            );
            execute_on_reset = std14::exchange( other.execute_on_reset, false );
        }

        return *this;
    }

    void reset() scope_noexcept
    {
        if ( execute_on_reset )
        {
            execute_on_reset = false;
            get_deleter()( get() );
        }
    }

    template< class RR >
    void reset( RR && r )
#if scope_CPP11_110
    {
        auto && guard = make_scope_fail( [&, this]{ get_deleter()(r); } ); // -Wunused-variable on clang

        reset();
        resource = conditional_forward<RR>( std::forward<RR>(r)
            , std11::bool_constant< std11::is_nothrow_assignable<R1, RR>::value >() );
        execute_on_reset = true;
    }
#else // scope_CPP11_110
    try
    {
        reset();
        resource = conditional_forward<RR>( std::forward<RR>(r)
            , std11::bool_constant< std11::is_nothrow_assignable<R1, RR>::value >() );
        execute_on_reset = true;
    }
    catch(...)
    {
        this->get_deleter()(r);
    }
#endif // scope_CPP11_110

    void release() scope_noexcept
    {
        execute_on_reset = false;
    }

    R1 const & get() const scope_noexcept
    {
        return resource;
    }

    // VC120/VS2013 produces ICE:

#if scope_HAVE( TRAILING_RETURN_TYPE ) && !scope_BETWEEN( scope_COMPILER_MSVC_VERSION, 120, 130 )
    template< class RR=R >
    auto operator*() const scope_noexcept ->
        scope_ENABLE_IF_R_(
            std::is_pointer<RR>::value && !std::is_void<typename std::remove_pointer<RR>::type>::value
            , typename std::add_lvalue_reference<typename std::remove_pointer<R>::type>::type
        )
#else
    typename std::add_lvalue_reference<typename std::remove_pointer<R>::type>::type
    operator*() const scope_noexcept
#endif
    {
        return *get();
    }

    // VC120/VS2013 produces ICE:

#if scope_HAVE( TRAILING_RETURN_TYPE ) && !scope_BETWEEN( scope_COMPILER_MSVC_VERSION, 120, 130 )
    template< class RR=R >
    auto operator->() const scope_noexcept -> scope_ENABLE_IF_R_( std::is_pointer<RR>::value, R )
#else
    R operator->() const scope_noexcept
#endif
    {
        return get();
    }

    D const & get_deleter() const scope_noexcept
    {
        return deleter;
    }

scope_is_delete_access:
	unique_resource & operator=( unique_resource const & ) scope_is_delete;
	unique_resource( unique_resource const & ) scope_is_delete;

private:
    R1 resource;
    D deleter;
    bool execute_on_reset;
};

#if scope_HAVE( DEDUCTION_GUIDES )
template< typename R, typename D >
unique_resource(R, D) -> unique_resource<R, D>;
#endif

// special factory function make_unique_resource_checked():

#if scope_HAVE_DEFAULT_FUNCTION_TEMPLATE_ARG

template< class R, class D, class S = typename std11::decay<R>::type >
unique_resource
<
    typename std11::decay<R>::type
    , typename std11::decay<D>::type
>
//make_unique_resource_checked( R && resource, typename std20::type_identity<S>::type const & invalid, D && deleter )
make_unique_resource_checked( R && resource, S const & invalid, D && deleter )
scope_noexcept_op
((
    std11::is_nothrow_constructible<typename std11::decay<R>::type, R>::value
    && std11::is_nothrow_constructible<typename std11::decay<D>::type, D>::value
))
{
    return unique_resource<typename std11::decay<R>::type, typename std11::decay<D>::type>(
        std::forward<R>( resource ), std::forward<D>( deleter ), !bool( resource == invalid ) );
}

#else // scope_HAVE_DEFAULT_FUNCTION_TEMPLATE_ARG

// avoid default template arguments:

template< class R, class D >
unique_resource
<
    typename std11::decay<R>::type
    , typename std11::decay<D>::type
>
make_unique_resource_checked( R && resource, R const & invalid, D && deleter )
scope_noexcept_op
((
    std11::is_nothrow_constructible<typename std11::decay<R>::type, R>::value
    && std11::is_nothrow_constructible<typename std11::decay<D>::type, D>::value
))
{
    return unique_resource<typename std11::decay<R>::type, typename std11::decay<D>::type>(
        std::forward<R>( resource ), std::forward<D>( deleter ), !bool( resource == invalid ) );
}

template< class R, class D, class S >
unique_resource
<
    typename std11::decay<R>::type
    , typename std11::decay<D>::type
>
make_unique_resource_checked( R && resource, S const & invalid, D && deleter )
scope_noexcept_op
((
    std11::is_nothrow_constructible<typename std11::decay<R>::type, R>::value
    && std11::is_nothrow_constructible<typename std11::decay<D>::type, D>::value
))
{
    return unique_resource<typename std11::decay<R>::type, typename std11::decay<D>::type>(
        std::forward<R>( resource ), std::forward<D>( deleter ), !bool( resource == invalid ) );
}

#endif // scope_HAVE_DEFAULT_FUNCTION_TEMPLATE_ARG

#else // #if scope_USE_POST_CPP98_VERSION

//
// C++98 version:
//

struct on_exit_policy
{
    mutable bool invoke_;

    on_exit_policy()
        : invoke_( true )
    {}

    on_exit_policy( on_exit_policy const & other )
        : invoke_( other.invoke_ )
    {
        other.invoke_ = false;
    }

    void release()
    {
        invoke_ = false;
    }

    bool perform()
    {
        return invoke_;
    }
};

struct on_fail_policy
{
    mutable int ucount_;

    on_fail_policy()
        : ucount_( detail::uncaught_exceptions() )
    {}

    on_fail_policy( on_fail_policy const & other )
        : ucount_( other.ucount_ )
    {
        other.ucount_ = std::numeric_limits<int>::max();
    }

    void release()
    {
        ucount_ = std::numeric_limits<int>::max();
    }

    bool perform()
    {
        return ucount_ < detail::uncaught_exceptions();
    }
};

struct on_success_policy
{
    mutable int ucount_;

    on_success_policy()
        : ucount_( detail::uncaught_exceptions() )
    {}

    on_success_policy( on_success_policy const & other )
        : ucount_( other.ucount_ )
    {
        other.ucount_ = -1;
    }

    void release()
    {
        ucount_ = -1;
    }

    bool perform()
    {
        return ucount_ >= detail::uncaught_exceptions();
    }
};

template< typename Policy, typename Action >
class scope_guard : public Policy
{
public:
    scope_guard( Action action )
        : Policy()
        , action_( action )
    {}

    scope_guard( scope_guard const & other )
        : Policy( other )
        , action_( other.action_ )
    {}

    virtual ~scope_guard()
    {
        if ( this->perform() )
            action_();
    }

private:
    scope_guard & operator=( scope_guard const & );

private:
    Action action_;
};

template< typename Fn = void(*)() >
class scope_exit : public scope_guard< on_exit_policy, Fn >
{
public:
    scope_exit( Fn action ) : scope_guard<on_exit_policy, Fn>( action ) {}
};

template< typename Fn = void(*)() >
class scope_fail : public scope_guard< on_fail_policy, Fn >
{
public:
    scope_fail( Fn action ) : scope_guard<on_fail_policy, Fn>( action ) {}
};

template< typename Fn = void(*)() >
class scope_success : public scope_guard< on_success_policy, Fn >
{
public:
    scope_success( Fn action ) : scope_guard<on_success_policy, Fn>( action ) {}
};

// unique_resource (C++98):

template< class R, class D >
class unique_resource
{
public:
    unique_resource()
        : resource()
        , deleter()
        , execute_on_reset( false )
    {}

    template< class RR, class DD >
    unique_resource( RR const & r, DD const & d, bool execute = true )
    : resource( r )
    , deleter(  d )
    , execute_on_reset( execute )
    {}

    // 'move' construction

    unique_resource( unique_resource const & other )
    : resource( other.resource )
    , deleter(  other.deleter  )
    , execute_on_reset( other.execute_on_reset )
    {
        other.execute_on_reset = false; // other.release();
    }

    ~unique_resource()
    {
        reset();
    }

    // 'move' assignment

    unique_resource & operator=( unique_resource const & other )
    {
        reset();
        resource = other.resource;
        deleter = other.deleter;
        execute_on_reset = other.execute_on_reset;
        other.execute_on_reset = false; // other.release();

        return *this;
    }

    void reset()
    {
        if ( execute_on_reset )
        {
            execute_on_reset = false;
            get_deleter()( get() );
        }
    }

    template< class RR >
    void reset( RR const & r )
    try
    {
        reset();
		resource = r;
        execute_on_reset = true;
    }
    catch(...)
    {
        this->get_deleter()( r );
    }

    void release()
    {
        execute_on_reset = false;
    }

    R const & get() const
    {
        return resource;
    }

    typename std11::remove_pointer<R>::type &
    operator*() const
    {
        return *get();
    }

    R operator->() const
    {
        return get();
    }

    D const & get_deleter() const
    {
        return deleter;
    }

private:
    // using R1 = conditional_t< is_reference_v<R>, reference_wrapper<remove_reference_t<R>>, R >; // exposition only
    // typedef R R1;
    R resource;
    D deleter;
    mutable bool execute_on_reset;
};

template< class EF >
scope_exit<EF> make_scope_exit( EF action )
{
    return scope_exit<EF>( action );
}

template< class EF >
scope_fail<EF> make_scope_fail( EF action )
{
    return scope_fail<EF>( action );
}

template< class EF >
scope_success<EF> make_scope_success( EF action )
{
    return scope_success<EF>( action );
}

template< class R, class D, class S >
unique_resource
<
    typename std11::decay<R>::type
    , typename std11::decay<D>::type
>
make_unique_resource_checked( R const & resource, S const & invalid, D const & deleter )
{
    return unique_resource<typename std11::decay<R>::type, typename std11::decay<D>::type>(
        resource,deleter, !bool( resource == invalid ) );
}

#endif // #if scope_USE_POST_CPP98_VERSION

}} // namespace nonstd::scope

//
// Make type available in namespace nonstd:
//

namespace nonstd
{
    using scope::scope_exit;
    using scope::scope_fail;
    using scope::scope_success;
    using scope::unique_resource;

    using scope::make_scope_exit;
    using scope::make_scope_fail;
    using scope::make_scope_success;
    using scope::make_unique_resource_checked;
}

#endif // scope_USES_STD_SCOPE

#endif // NONSTD_SCOPE_LITE_HPP
