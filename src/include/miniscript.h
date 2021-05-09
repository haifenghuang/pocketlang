/*
 *  Copyright (c) 2021 Thakee Nathees
 *  Licensed under: MIT License
 */

#ifndef MINISCRIPT_H
#define MINISCRIPT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// The version number macros.
#define MS_VERSION_MAJOR 0
#define MS_VERSION_MINOR 1
#define MS_VERSION_PATCH 0

// String representation of the value.
#define MS_VERSION_STRING "0.1.0"

// miniscript visibility macros. define MS_DLL for using miniscript as a 
// shared library and define MS_COMPILE to export symbols.

#ifdef _MSC_VER
  #define _MS_EXPORT __declspec(dllexport)
  #define _MS_IMPORT __declspec(dllimport)
#elif defined(__GNUC__)
  #define _MS_EXPORT __attribute__((visibility ("default")))
  #define _MS_IMPORT
#else
  #define _MS_EXPORT
  #define _MS_IMPORT
#endif

#ifdef MS_DLL
  #ifdef MS_COMPILE
    #define MS_PUBLIC _MS_EXPORT
  #else
    #define MS_PUBLIC _MS_IMPORT
  #endif
#else
  #define MS_PUBLIC
#endif

// MiniScript Virtual Machine.
// it'll contain the state of the execution, stack, heap, and manage memory
// allocations.
typedef struct MSVM MSVM;

// Nan-Tagging could be disable for debugging/portability purposes only when
// compiling the compiler. Do not change this if using the miniscript library
// for embedding. To disable when compiling the compiler, define
// `VAR_NAN_TAGGING 0`, otherwise it defaults to Nan-Tagging.
#ifndef VAR_NAN_TAGGING
  #define VAR_NAN_TAGGING 1
#endif

#if VAR_NAN_TAGGING
  typedef uint64_t Var;
#else
  typedef struct Var Var;
#endif

// A function that'll be called for all the allocation calls by MSVM.
//
// - To allocate new memory it'll pass NULL to parameter [memory] and the
//   required size to [new_size]. On failure the return value would be NULL.
//
// - When reallocating an existing memory if it's grow in place the return
//   address would be the same as [memory] otherwise a new address.
//
// - To free an allocated memory pass [memory] and 0 to [new_size]. The
//   function will return NULL.
typedef void* (*msReallocFn)(void* memory, size_t new_size, void* user_data);

// C function pointer which is callable from MiniScript.
typedef void (*msNativeFn)(MSVM* vm);

typedef enum {

  // Compile time errors (syntax errors, unresolved fn, etc).
  MS_ERROR_COMPILE,

  // Runtime error message.
  MS_ERROR_RUNTIME,

  // One entry of a runtime error stack.
  MS_ERROR_STACKTRACE,
} MSErrorType;

// Error callback function pointer. for runtime error it'll call first with
// MS_ERROR_RUNTIME followed by multiple callbacks with MS_ERROR_STACKTRACE.
typedef void (*msErrorFn) (MSVM* vm, MSErrorType type,
                           const char* file, int line,
                           const char* message);

// A function callback used by `print()` statement.
typedef void (*msWriteFn) (MSVM* vm, const char* text);

typedef struct msStringResult msStringResult;

// A function callback symbol for clean/free the msStringResult.
typedef void (*msResultDoneFn) (MSVM* vm, msStringResult result);

// Result of the MiniScriptLoadScriptFn function.
struct msStringResult {
  bool success;           //< State of the result.
  const char* string;     //< The string result.
  void* user_data;        //< User related data.
  msResultDoneFn on_done; //< Called once vm done with the string.
};

// A function callback to resolve the import script name from the [from] path
// to an absolute (or relative to the cwd). This is required to solve same
// script imported with different relative path.
// Note: If the name is the root script [from] would be NULL.
typedef msStringResult (*msResolvePathFn) (MSVM* vm, const char* from,
                                        const char* name);

// Load and return the script. Called by the compiler to fetch initial source
// code and source for import statements.
typedef msStringResult (*msLoadScriptFn) (MSVM* vm, const char* path);

// This function will be called once it done with the loaded script only if
// it's corresponding MSLoadScriptResult is succeeded (ie. is_failed = false).
//typedef void (*msLoadDoneFn) (MSVM* vm, msStringResult result);

typedef struct {

  // The callback used to allocate, reallocate, and free. If the function
  // pointer is NULL it defaults to the VM's realloc(), free() wrappers.
  msReallocFn realloc_fn;

  msErrorFn error_fn;
  msWriteFn write_fn;

  msResolvePathFn resolve_path_fn;
  msLoadScriptFn load_script_fn;

  // User defined data associated with VM.
  void* user_data;

} msConfiguration;

// Initialize the configuration and set ALL of it's values to the defaults.
// Call this before setting any particular field of it.
MS_PUBLIC void msInitConfiguration(msConfiguration* config);

typedef enum {
  RESULT_SUCCESS = 0,
  RESULT_COMPILE_ERROR,
  RESULT_RUNTIME_ERROR,
} MSInterpretResult;

// Allocate initialize and returns a new VM
MS_PUBLIC MSVM* msNewVM(msConfiguration* config);

// Clean the VM and dispose all the resources allocated by the VM.
MS_PUBLIC void msFreeVM(MSVM* vm);

// Compile and execut file at given path.
MS_PUBLIC MSInterpretResult msInterpret(MSVM* vm, const char* file);

// Set a runtime error to vm.
MS_PUBLIC void msSetRuntimeError(MSVM* vm, const char* format, ...);

// Returns the associated user data.
MS_PUBLIC void* msGetUserData(MSVM* vm);

// Update the user data of the vm.
MS_PUBLIC void msSetUserData(MSVM* vm, void* user_data);

// Encode types to var.
// TODO: user need to use vmPushTempRoot() for strings.
MS_PUBLIC Var msVarBool(MSVM* vm, bool value);
MS_PUBLIC Var msVarNumber(MSVM* vm, double value);
MS_PUBLIC Var msVarString(MSVM* vm, const char* value);

// Decode var types.
// TODO: const char* should be copied otherwise it'll become dangling pointer.
MS_PUBLIC bool msAsBool(MSVM* vm, Var value);
MS_PUBLIC double msAsNumber(MSVM* vm, Var value);
MS_PUBLIC const char* msAsString(MSVM* vm, Var value);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MINISCRIPT_H