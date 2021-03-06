/*
 * Copyright (c) 1987, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_malloc.c	8.3 (Berkeley) 1/4/94
 * $Id: kern_malloc.c,v 1.51.2.2 1999/01/22 05:17:56 dillon Exp $
 */

#include "opt_vm.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#define MALLOC_INSTANTIATE
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/vmmeter.h>
#include <sys/lock.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>
#ifndef OSKIT
#include <vm/pmap.h>
#include <vm/vm_map.h>

static void kmeminit __P((void *));
SYSINIT(kmem, SI_SUB_KMEM, SI_ORDER_FIRST, kmeminit, NULL)
#endif

static MALLOC_DEFINE(M_FREE, "free", "should be on free list");

static struct malloc_type *kmemstatistics;
#ifdef OSKIT
struct kmembuckets bucket[MINBUCKET + 16];
struct kmemusage *kmemusage;
char *kmembase;
char *kmemlimit;
#else /* !OSKIT */
static struct kmembuckets bucket[MINBUCKET + 16];
static struct kmemusage *kmemusage;
static char *kmembase;
static char *kmemlimit;
#endif /* !OSKIT */
static int vm_kmem_size;

#ifdef INVARIANTS
/*
 * This structure provides a set of masks to catch unaligned frees.
 */
static long addrmask[] = { 0,
	0x00000001, 0x00000003, 0x00000007, 0x0000000f,
	0x0000001f, 0x0000003f, 0x0000007f, 0x000000ff,
	0x000001ff, 0x000003ff, 0x000007ff, 0x00000fff,
	0x00001fff, 0x00003fff, 0x00007fff, 0x0000ffff,
};

/*
 * The WEIRD_ADDR is used as known text to copy into free objects so
 * that modifications after frees can be detected.
 */
#define WEIRD_ADDR	0xdeadc0de
#define MAX_COPY	64

/*
 * Normally the first word of the structure is used to hold the list
 * pointer for free objects. However, when running with diagnostics,
 * we use the third and fourth fields, so as to catch modifications
 * in the most commonly trashed first two words.
 */
struct freelist {
	long	spare0;
	struct malloc_type *type;
	long	spare1;
	caddr_t	next;
};
#else /* !INVARIANTS */
struct freelist {
	caddr_t	next;
};
#endif /* INVARIANTS */

/*
 * Allocate a block of memory
 */
void *
malloc(size, type, flags)
	unsigned long size;
	struct malloc_type *type;
	int flags;
{
	register struct kmembuckets *kbp;
	register struct kmemusage *kup;
	register struct freelist *freep;
	long indx, npg, allocsize;
	int s;
	caddr_t va, cp, savedlist;
#ifdef INVARIANTS
	long *end, *lp;
	int copysize;
	char *savedtype;
#endif
	register struct malloc_type *ksp = type;

	/*
	 * Must be at splmem() prior to initializing segment to handle
	 * potential initialization race.
	 */

	s = splmem();

	if (!type->ks_next)
		malloc_init(type);

	indx = BUCKETINDX(size);
	kbp = &bucket[indx];
#ifndef OSKIT
	while (ksp->ks_memuse >= ksp->ks_limit) {
		if (flags & M_NOWAIT) {
			splx(s);
			return ((void *) NULL);
		}
		if (ksp->ks_limblocks < 65535)
			ksp->ks_limblocks++;
		tsleep((caddr_t)ksp, PSWP+2, type->ks_shortdesc, 0);
	}
#endif
	ksp->ks_size |= 1 << indx;
#ifdef INVARIANTS
	copysize = 1 << indx < MAX_COPY ? 1 << indx : MAX_COPY;
#endif
	if (kbp->kb_next == NULL) {
		kbp->kb_last = NULL;
		if (size > MAXALLOCSAVE)
			allocsize = roundup(size, PAGE_SIZE);
		else
			allocsize = 1 << indx;
		npg = btoc(allocsize);
		va = (caddr_t) kmem_malloc(kmem_map, (vm_size_t)ctob(npg), flags);
		if (va == NULL) {
			splx(s);
			return ((void *) NULL);
		}
		kbp->kb_total += kbp->kb_elmpercl;
		kup = btokup(va);
		kup->ku_indx = indx;
		if (allocsize > MAXALLOCSAVE) {
			if (npg > 65535)
				panic("malloc: allocation too large");
			kup->ku_pagecnt = npg;
			ksp->ks_memuse += allocsize;
			goto out;
		}
		kup->ku_freecnt = kbp->kb_elmpercl;
		kbp->kb_totalfree += kbp->kb_elmpercl;
		/*
		 * Just in case we blocked while allocating memory,
		 * and someone else also allocated memory for this
		 * bucket, don't assume the list is still empty.
		 */
		savedlist = kbp->kb_next;
		kbp->kb_next = cp = va + (npg * PAGE_SIZE) - allocsize;
		for (;;) {
			freep = (struct freelist *)cp;
#ifdef INVARIANTS
			/*
			 * Copy in known text to detect modification
			 * after freeing.
			 */
			end = (long *)&cp[copysize];
			for (lp = (long *)cp; lp < end; lp++)
				*lp = WEIRD_ADDR;
			freep->type = M_FREE;
#endif /* INVARIANTS */
			if (cp <= va)
				break;
			cp -= allocsize;
			freep->next = cp;
		}
		freep->next = savedlist;
		if (kbp->kb_last == NULL)
			kbp->kb_last = (caddr_t)freep;
	}
	va = kbp->kb_next;
	kbp->kb_next = ((struct freelist *)va)->next;
#ifdef INVARIANTS
	freep = (struct freelist *)va;
	savedtype = (char *) type->ks_shortdesc;
#if BYTE_ORDER == BIG_ENDIAN
	freep->type = (struct malloc_type *)WEIRD_ADDR >> 16;
#endif
#if BYTE_ORDER == LITTLE_ENDIAN
	freep->type = (struct malloc_type *)WEIRD_ADDR;
#endif
	if ((intptr_t)(void *)&freep->next & 0x2)
		freep->next = (caddr_t)((WEIRD_ADDR >> 16)|(WEIRD_ADDR << 16));
	else
		freep->next = (caddr_t)WEIRD_ADDR;
	end = (long *)&va[copysize];
	for (lp = (long *)va; lp < end; lp++) {
		if (*lp == WEIRD_ADDR)
			continue;
		printf("%s %ld of object %p size %lu %s %s (0x%lx != 0x%lx)\n",
			"Data modified on freelist: word",
			(long)(lp - (long *)va), (void *)va, size,
			"previous type", savedtype, *lp, (u_long)WEIRD_ADDR);
		break;
	}
	freep->spare0 = 0;
#endif /* INVARIANTS */
	kup = btokup(va);
	if (kup->ku_indx != indx)
		panic("malloc: wrong bucket");
	if (kup->ku_freecnt == 0)
		panic("malloc: lost data");
	kup->ku_freecnt--;
	kbp->kb_totalfree--;
	ksp->ks_memuse += 1 << indx;
out:
	kbp->kb_calls++;
	ksp->ks_inuse++;
	ksp->ks_calls++;
	if (ksp->ks_memuse > ksp->ks_maxused)
		ksp->ks_maxused = ksp->ks_memuse;
	splx(s);
	return ((void *) va);
}

/*
 * Free a block of memory allocated by malloc.
 */
void
free(addr, type)
	void *addr;
	struct malloc_type *type;
{
	register struct kmembuckets *kbp;
	register struct kmemusage *kup;
	register struct freelist *freep;
	long size;
	int s;
#ifdef INVARIANTS
	struct freelist *fp;
	long *end, *lp, alloc, copysize;
#endif
	register struct malloc_type *ksp = type;

	if (!type->ks_next)
		panic("freeing with unknown type (%s)", type->ks_shortdesc);

	KASSERT(kmembase <= (char *)addr && (char *)addr < kmemlimit,
	    ("free: address %p out of range", (void *)addr));
	kup = btokup(addr);
	size = 1 << kup->ku_indx;
	kbp = &bucket[kup->ku_indx];
	s = splmem();
#ifdef INVARIANTS
	/*
	 * Check for returns of data that do not point to the
	 * beginning of the allocation.
	 */
	if (size > PAGE_SIZE)
		alloc = addrmask[BUCKETINDX(PAGE_SIZE)];
	else
		alloc = addrmask[kup->ku_indx];
	if (((uintptr_t)(void *)addr & alloc) != 0)
		panic("free: unaligned addr %p, size %ld, type %s, mask %ld",
		    (void *)addr, size, type->ks_shortdesc, alloc);
#endif /* INVARIANTS */
	if (size > MAXALLOCSAVE) {
		kmem_free(kmem_map, (vm_offset_t)addr, ctob(kup->ku_pagecnt));
		size = kup->ku_pagecnt << PAGE_SHIFT;
		ksp->ks_memuse -= size;
		kup->ku_indx = 0;
		kup->ku_pagecnt = 0;
		if (ksp->ks_memuse + size >= ksp->ks_limit &&
		    ksp->ks_memuse < ksp->ks_limit)
			wakeup((caddr_t)ksp);
		ksp->ks_inuse--;
		kbp->kb_total -= 1;
		splx(s);
		return;
	}
	freep = (struct freelist *)addr;
#ifdef INVARIANTS
	/*
	 * Check for multiple frees. Use a quick check to see if
	 * it looks free before laboriously searching the freelist.
	 */
	if (freep->spare0 == WEIRD_ADDR) {
		fp = (struct freelist *)kbp->kb_next;
		while (fp) {
			if (fp->spare0 != WEIRD_ADDR)
				panic("free: free item %p modified", fp);
			else if (addr == (caddr_t)fp)
				panic("free: multiple freed item %p", addr);
			fp = (struct freelist *)fp->next;
		}
	}
	/*
	 * Copy in known text to detect modification after freeing
	 * and to make it look free. Also, save the type being freed
	 * so we can list likely culprit if modification is detected
	 * when the object is reallocated.
	 */
	copysize = size < MAX_COPY ? size : MAX_COPY;
	end = (long *)&((caddr_t)addr)[copysize];
	for (lp = (long *)addr; lp < end; lp++)
		*lp = WEIRD_ADDR;
	freep->type = type;
#endif /* INVARIANTS */
	kup->ku_freecnt++;
	if (kup->ku_freecnt >= kbp->kb_elmpercl)
		if (kup->ku_freecnt > kbp->kb_elmpercl)
			panic("free: multiple frees");
		else if (kbp->kb_totalfree > kbp->kb_highwat)
			kbp->kb_couldfree++;
	kbp->kb_totalfree++;
	ksp->ks_memuse -= size;
	if (ksp->ks_memuse + size >= ksp->ks_limit &&
	    ksp->ks_memuse < ksp->ks_limit)
		wakeup((caddr_t)ksp);
	ksp->ks_inuse--;
#ifdef OLD_MALLOC_MEMORY_POLICY
	if (kbp->kb_next == NULL)
		kbp->kb_next = addr;
	else
		((struct freelist *)kbp->kb_last)->next = addr;
	freep->next = NULL;
	kbp->kb_last = addr;
#else
	/*
	 * Return memory to the head of the queue for quick reuse.  This
	 * can improve performance by improving the probability of the
	 * item being in the cache when it is reused.
	 */
	if (kbp->kb_next == NULL) {
		kbp->kb_next = addr;
		kbp->kb_last = addr;
		freep->next = NULL;
	} else {
		freep->next = kbp->kb_next;
		kbp->kb_next = addr;
	}
#endif
	splx(s);
}


/*
 * this is all completely bogus anyway in our model
 * 
 * kmemlimit
 */
/*
 * Initialize the kernel memory allocator
 */
/* ARGSUSED*/
#ifdef OSKIT
void
#else
static void
#endif
kmeminit(dummy)
	void *dummy;
{
	register long indx;
#ifndef OSKIT
	int npg;
	int mem_size;
	int xvm_kmem_size;

#if	((MAXALLOCSAVE & (MAXALLOCSAVE - 1)) != 0)
#error "kmeminit: MAXALLOCSAVE not power of 2"
#endif
#if	(MAXALLOCSAVE > MINALLOCSIZE * 32768)
#error "kmeminit: MAXALLOCSAVE too big"
#endif
#if	(MAXALLOCSAVE < PAGE_SIZE)
#error "kmeminit: MAXALLOCSAVE too small"
#endif

	/*
	 * Try to auto-tune the kernel memory size, so that it is
	 * more applicable for a wider range of machine sizes.
	 * On an X86, a VM_KMEM_SIZE_SCALE value of 4 is good, while
	 * a VM_KMEM_SIZE of 12MB is a fair compromise.  The
	 * VM_KMEM_SIZE_MAX is dependent on the maximum KVA space
	 * available, and on an X86 with a total KVA space of 256MB,
	 * try to keep VM_KMEM_SIZE_MAX at 80MB or below.
	 *
	 * Note that the kmem_map is also used by the zone allocator,
	 * so make sure that there is enough space.
	 */
	vm_kmem_size = VM_KMEM_SIZE;
	mem_size = cnt.v_page_count * PAGE_SIZE;

#if defined(VM_KMEM_SIZE_SCALE)
	if ((mem_size / VM_KMEM_SIZE_SCALE) > vm_kmem_size)
		vm_kmem_size = mem_size / VM_KMEM_SIZE_SCALE;
#endif

#if defined(VM_KMEM_SIZE_MAX)
	if (vm_kmem_size >= VM_KMEM_SIZE_MAX)
		vm_kmem_size = VM_KMEM_SIZE_MAX;
#endif

	/* Allow final override from the kernel environment */
	if (getenv_int("kern.vm.kmem.size", &xvm_kmem_size))
	    vm_kmem_size = xvm_kmem_size;

	if (vm_kmem_size > 2 * (cnt.v_page_count * PAGE_SIZE))
		vm_kmem_size = 2 * (cnt.v_page_count * PAGE_SIZE);

	npg = (nmbufs * MSIZE + nmbclusters * MCLBYTES + vm_kmem_size)
		/ PAGE_SIZE;

	kmemusage = (struct kmemusage *) kmem_alloc(kernel_map,
		(vm_size_t)(npg * sizeof(struct kmemusage)));
	kmem_map = kmem_suballoc(kernel_map, (vm_offset_t *)&kmembase,
		(vm_offset_t *)&kmemlimit, (vm_size_t)(npg * PAGE_SIZE));
	kmem_map->system_map = 1;
#endif /* !OSKIT */
	for (indx = 0; indx < MINBUCKET + 16; indx++) {
		if (1 << indx >= PAGE_SIZE)
			bucket[indx].kb_elmpercl = 1;
		else
			bucket[indx].kb_elmpercl = PAGE_SIZE / (1 << indx);
		bucket[indx].kb_highwat = 5 * bucket[indx].kb_elmpercl;
	}
}

void
malloc_init(data)
	void *data;
{
	struct malloc_type *type = (struct malloc_type *)data;

	if (type->ks_magic != M_MAGIC) 
		panic("malloc type lacks magic");

	if (type->ks_next)
		return;

#ifndef OSKIT
	if (cnt.v_page_count == 0)
		panic("malloc_init not allowed before vm init");
#endif

	/*
	 * The default limits for each malloc region is 1/2 of the
	 * malloc portion of the kmem map size.
	 */
	type->ks_limit = vm_kmem_size / 2;
	type->ks_next = kmemstatistics;	
	kmemstatistics = type;
}

void
malloc_uninit(data)
	void *data;
{
	struct malloc_type *type = (struct malloc_type *)data;
	struct malloc_type *t;

	if (type->ks_magic != M_MAGIC) 
		panic("malloc type lacks magic");

#ifndef OSKIT
	if (cnt.v_page_count == 0)
		panic("malloc_uninit not allowed before vm init");
#endif

	if (type == kmemstatistics)
		kmemstatistics = type->ks_next;
	else {
		for (t = kmemstatistics; t->ks_next != NULL; t = t->ks_next) {
			if (t->ks_next == type) {
				t->ks_next = type->ks_next;
				break;
			}
		}
	}
}

