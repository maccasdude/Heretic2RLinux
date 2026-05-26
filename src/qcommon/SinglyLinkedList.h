//
// SinglyLinkedList.h
//
// Copyright 1998 Raven Software
//

#pragma once

#include "H2Common.h"
#include "GenericUnions.h"

// The SLL node struct - moved out of SinglyLinkedList.c so callers can use
// sizeof() to allocate correct space. On 32-bit MSVC this was 8 bytes
// (4-byte GenericUnion4 + 4-byte pointer); on 64-bit GCC pointer types in
// GenericUnion4 make it 8 bytes wide and the struct becomes 16 bytes.
typedef struct SinglyLinkedListNode_s
{
	union GenericUnion4_u data;
	struct SinglyLinkedListNode_s* next;
} SinglyLinkedListNode_t;

// SLL_NODE_SIZE was a literal `8` (sizeof on 32-bit Windows). On 64-bit it's
// `16`. Use sizeof() so it's correct on every platform.
#define SLL_NODE_SIZE			(sizeof(SinglyLinkedListNode_t))
#define SLL_NODE_BLOCK_SIZE		256 //mxd

typedef struct SinglyLinkedList_s
{
	struct SinglyLinkedListNode_s* rearSentinel;
	struct SinglyLinkedListNode_s* front;
	struct SinglyLinkedListNode_s* current;
} SinglyLinkedList_t;

H2COMMON_API void SLList_DefaultCon(SinglyLinkedList_t* this_ptr);
H2COMMON_API void SLList_Des(SinglyLinkedList_t* this_ptr);
H2COMMON_API qboolean SLList_AtEnd(const SinglyLinkedList_t* this_ptr);
H2COMMON_API qboolean SLList_AtLast(const SinglyLinkedList_t* this_ptr);
H2COMMON_API qboolean SLList_IsEmpty(const SinglyLinkedList_t* this_ptr);
H2COMMON_API GenericUnion4_t SLList_Increment(SinglyLinkedList_t* this_ptr);
H2COMMON_API GenericUnion4_t SLList_PostIncrement(SinglyLinkedList_t* this_ptr);
H2COMMON_API GenericUnion4_t SLList_Front(SinglyLinkedList_t* this_ptr);
H2COMMON_API GenericUnion4_t SLList_ReplaceCurrent(const SinglyLinkedList_t* this_ptr, GenericUnion4_t to_replace);
H2COMMON_API void SLList_PushEmpty(SinglyLinkedList_t* this_ptr);
H2COMMON_API void SLList_Push(SinglyLinkedList_t* this_ptr, GenericUnion4_t to_insert);
H2COMMON_API GenericUnion4_t SLList_Pop(SinglyLinkedList_t* this_ptr);
H2COMMON_API void SLList_Chop(SinglyLinkedList_t* this_ptr);
H2COMMON_API void SLList_InsertAfter(const SinglyLinkedList_t* this_ptr, GenericUnion4_t to_insert);