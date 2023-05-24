#pragma once

#include <stdint.h>
#include <stddef.h>
#include <cstring>

namespace nakamir {

#if !defined(nak_DEBUG_MEM)

	// Safer memory allocation functions, will kill the app on failure.
	void* nak_malloc(size_t bytes);
	void* nak_calloc(size_t bytes);
	void* nak_realloc(void* memory, size_t bytes);
	void  _nak_free(void* memory);

#define nak_free(memory) { _nak_free(memory); memory = nullptr; };

#define nak_malloc_t(T, count) ((T*)nak_malloc ((count) * sizeof(T)))
#define nak_calloc_t(T, count) ((T*)nak_calloc((count) * sizeof(T)))
#define nak_realloc_t(T, memory, count) ((T*)nak_realloc(memory, (count) * sizeof(T)))

#else

	// Safer memory allocation functions, will kill the app on failure.
	void* nak_malloc_d(size_t bytes, const char* type, const char* filename, int32_t line);
	void* nak_calloc_d(size_t bytes, const char* type, const char* filename, int32_t line);
	void* nak_realloc_d(void* memory, size_t bytes, const char* type, const char* filename, int32_t line);
	void  _nak_free_d(void* memory, const char* filename, int32_t line);

#define nak_malloc(bytes) nak_malloc_d(bytes, "raw", __FILE__, __LINE__)
#define nak_calloc(bytes) nak_calloc_d(bytes, "raw", __FILE__, __LINE__)
#define nak_realloc(memory, bytes) nak_realloc_d(memory, bytes, "raw", __FILE__, __LINE__)
#define _nak_free(memory) _nak_free_d(memory, __FILE__, __LINE__)

#define nak_free(memory) { _nak_free_d(memory, __FILE__, __LINE__); memory = nullptr; };

#define nak_malloc_t(T, count) ((T*)nak_malloc_d((count) * sizeof(T), #T, __FILE__, __LINE__))
#define nak_calloc_t(T, count) ((T*)nak_calloc_d((count) * sizeof(T), #T, __FILE__, __LINE__))
#define nak_realloc_t(T, memory, count) ((T*)nak_realloc_d(memory, (count) * sizeof(T), #T, __FILE__, __LINE__))

#endif

	void nak_mem_log_allocations();

} // namespace nakamir
