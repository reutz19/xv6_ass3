#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()
struct segdesc gdt[NSEGS];

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpunum()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);

  // Map cpu, and curproc
  c->gdt[SEG_KCPU] = SEG(STA_W, &c->cpu, 8, 0);

  lgdt(c->gdt, sizeof(c->gdt));
  loadgs(SEG_KCPU << 3);
  
  // Initialize cpu-local storage.
  cpu = c;
  proc = 0;
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)p2v(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table 
    // entries, if necessary.
    *pde = v2p(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;
  
  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}


// ----------------   manage pages  ----------------------

#ifndef SELECTION_NONE

int idx_page_out_FIFO(void);
uint idx_page_out_NFU(void);
void update_refer_pages(void);

int 
get_pgidx_in_pysc(void* va)
{
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++){
    if (proc->pysc_pgmd[i].pva == va)
      return i;
  }
  return -1;
}

int
get_pgidx_in_swap(void* va)
{
  int i;
  for (i = 0; i < MAX_FILE_PAGES; i++){
    if (proc->swap_pgmd[i].pva == va)
      return i;
  }
  return -1;
}

int 
get_free_pgidx_swap(void)
{
  int i;
  for (i = 0; i < MAX_FILE_PAGES; i++){
    if (proc->swap_pgmd[i].pva == (void*)-1)
      return i;
  }
  return -1;
}

int 
get_free_pgidx_pysc(void)
{
  int i = 0;
  //#if defined(SELECTION_FIFO) || defined(SELECTION_SCFIFO) 
    i = proc->oldest_pgidx;
  //#endif

  for (; i < MAX_PSYC_PAGES; i++)
  {
    if (proc->pysc_pgmd[i].pva == (void*)-1)
      return i;
  }
  return -1;
}

void 
init_pysc_pgmd(struct proc *p)
{
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++)
  {
    p->pysc_pgmd[i].pva = (void*)-1;
  	p->pysc_pgmd[i].counter = 0;
  }
}

void 
init_swap_pgmd(struct proc *p)
{
  int i;
  for (i = 0; i < MAX_FILE_PAGES; i++)
  {
    p->swap_pgmd[i].pva = (void*)-1;
    p->swap_pgmd[i].counter = 0;
  }
}

void 
copy_pysc_pgmd(struct proc *dstp, struct proc *srcp)
{
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++)
  {
    dstp->pysc_pgmd[i].pva = srcp->pysc_pgmd[i].pva;
  	dstp->pysc_pgmd[i].counter = srcp->pysc_pgmd[i].counter;
  }
}

void 
copy_swap_pgmd(struct proc *dstp, struct proc *srcp)
{
  int i;
  for (i = 0; i < MAX_FILE_PAGES; i++)
  {
    dstp->swap_pgmd[i].pva = srcp->swap_pgmd[i].pva;
    dstp->swap_pgmd[i].counter = srcp->swap_pgmd[i].counter;
  }
}

//the function run a full circle from oldest_pgidx and block all holls by moving celles back
void 
clear_pysc_and_block_holls(uint idx)
{
  int i = 0,j;
  //set pysc_pgmd
  proc->pysc_pgmd[idx].pva = (void*)-1;
  proc->pysc_pgmd[idx].counter = 0;

  // remove hols
  int oldest = proc->oldest_pgidx;
  int curr_idx, curr, next;
  int diff = 1;
  while (i < MAX_FILE_PAGES-diff)
  {
    curr_idx = (oldest + i) % MAX_PSYC_PAGES;

    //found a holl
    while (proc->pysc_pgmd[curr_idx].pva == (void*)-1)
    {
      //for each till end do pysc_pgmd[curr]=pysc_pgmd[curr+1]
      curr = curr_idx;
      for (j = 0; j < MAX_FILE_PAGES-diff-i; j++){
        next = (curr+1) % MAX_PSYC_PAGES;
        proc->pysc_pgmd[curr] = proc->pysc_pgmd[next];
        curr = next;
      }
      //clean last cell
      proc->pysc_pgmd[curr].counter = 0;
      proc->pysc_pgmd[curr].pva = (void*)-1;
      diff++;
    }
    i++;
  }
}

// -----------   manage page replacemaent schemes --------

#if defined(SELECTION_FIFO) || defined(SELECTION_SCFIFO) 

int
idx_page_out_FIFO(void)
{
	int ret_index = proc->oldest_pgidx;
  /*int i=0; 
  cprintf("process->pid: %d  proc->oldest_pgidx: %d\n", proc->pid, proc->oldest_pgidx);
  while(i<15){
    cprintf("pysc_pgmd[%d] = %x\n",i, proc->pysc_pgmd[i].pva);
    i++;
  }
  i=0;
  while(i<15){
    cprintf("swap_pgmd[%d] = %x\n",i, proc->swap_pgmd[i].pva);
    i++;
  } 
  */

	#ifdef SELECTION_SCFIFO

  pte_t* pte;

  // until he find the next page in the "queue"
  for(;;)
  {   

    //address of page out
   	pte = walkpgdir(proc->pgdir, proc->pysc_pgmd[ret_index].pva, 0);
    if (!(*pte & PTE_A)){
    	//found
    	proc->oldest_pgidx = ret_index;
    	break;
    }

  	// set off Reference bit and continue to next page
    *pte = *pte & ~PTE_A; 

    //updape oldest_pgidx counter
    ret_index = (ret_index + 1) % MAX_PSYC_PAGES;
  }

  #else
  	//update oldest_pgidx counter
  	proc->oldest_pgidx = (ret_index + 1) % MAX_PSYC_PAGES;

  #endif

  //cprintf("in idx_page_out_FIFO: ret_index=%d\n",ret_index);
  return ret_index;
}

#elif defined(SELECTION_DEFAULT) || defined(SELECTION_NFU) 

uint
idx_page_out_NFU(void)
{  
	uint min_index = 0;
	int i;
  uint min_count = 0xffffffff;
  //pte_t* pte;

  min_count = proc->pysc_pgmd[0].counter;

	for (i = 0; i < MAX_PSYC_PAGES; i++)
	{
    if(proc->pysc_pgmd[i].pva == (void*)-1)
      continue;
		if(proc->pysc_pgmd[i].counter < min_count)
		{
			min_count = proc->pysc_pgmd[i].counter;
			min_index = i;
		}
    // TODO: 1. proc->pysc_pgmd[i].counter = 0
    
	}

	return min_index;
}

#endif

void
update_refer_pages(void)
{
  int i;
  pte_t* pte;
	for (i = 0; i < MAX_PSYC_PAGES; i++)
	{
		pte = walkpgdir(proc->pgdir, proc->pysc_pgmd[i].pva, 0); 

		if(*pte & PTE_A)
		{
			proc->pysc_pgmd[i].counter++;
			*pte = *pte & ~PTE_A; 
		}
	}
}

// select a page from pysc memory and paged it out, if given swap_pgidx >=0 then locate the paged out page in swap_pgidx
// return the index of the freed pysc index page
int 
paged_out(int swap_pgidx)
{
  //cprintf("i'm in paged_out\n");
  int pysc_pgidx = 0;
  	
  #if defined(SELECTION_FIFO) || defined(SELECTION_SCFIFO) 
  	pysc_pgidx = idx_page_out_FIFO();
  #elif defined(SELECTION_DEFAULT) || defined(SELECTION_NFU) 
  	pysc_pgidx = idx_page_out_NFU();
  #endif

  pte_t* outpg_va = proc->pysc_pgmd[pysc_pgidx].pva;
  //cprintf("in paged_out: outpg_va=%x   before KFREE\n", outpg_va);
  pte_t* pte = walkpgdir(proc->pgdir, outpg_va, 0); //return pointer to page on pysc memory
  
  if (swap_pgidx < 0){
  // look for a free space on swap file
    if ((swap_pgidx = get_free_pgidx_swap()) == -1) {
      panic("paged_out: more then 30 pages for process");
      return -1;
    }
  }

  uint pgoff = ((uint)swap_pgidx) * PGSIZE;
  writeToSwapFile(proc, ((char*)proc->pysc_pgmd[pysc_pgidx].pva), pgoff, PGSIZE);
  proc->swap_pgmd[swap_pgidx].pva = outpg_va;
  proc->pgout_num++;
  
  char * v = p2v(PTE_ADDR(*pte));
  kfree(v);   							// free pysical memory
  //cprintf("in paged_out:swap_pgidx = %d     after KFREE\n", swap_pgidx);
  lcr3(v2p(proc->pgdir));   // update pgdir after page out

  clear_pysc_and_block_holls(pysc_pgidx);
  //proc->pysc_pgmd[pysc_pgidx].pva = (void*)-1;
  *pte = *pte & ~PTE_P;     // set not present
  *pte = *pte | PTE_PG;     // set swapped out

  //cprintf("in paged_out:after writeToSwapFile\n");
  return pysc_pgidx;
}

#endif


// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
// 
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP, 
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (p2v(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start, 
                (uint)k->phys_start, k->perm) < 0)
      return 0;
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(v2p(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  pushcli();
  cpu->gdt[SEG_TSS] = SEG16(STS_T32A, &cpu->ts, sizeof(cpu->ts)-1, 0);
  cpu->gdt[SEG_TSS].s = 0;
  cpu->ts.ss0 = SEG_KDATA << 3;
  cpu->ts.esp0 = (uint)proc->kstack + KSTACKSIZE;
  ltr(SEG_TSS << 3);
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");
  lcr3(v2p(p->pgdir));  // switch to new address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;
  
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, v2p(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, p2v(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;
	
  #ifndef SELECTION_NONE
  int ava_pyscidx = -1;
  #endif

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    
    #ifndef SELECTION_NONE
      if (proc->pid > 2 && (ava_pyscidx = get_free_pgidx_pysc()) == -1){
        ava_pyscidx = paged_out(-1); // page out from pysc memory to swap
      }
    #endif

    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }

    memset(mem, 0, PGSIZE);
    mappages(pgdir, (char*)a, PGSIZE, v2p(mem), PTE_W|PTE_U);
    
    #ifndef SELECTION_NONE
      if (ava_pyscidx >= 0){
      	//cprintf("pid=%d allocuvm: pysc[%d]=%p --> pysc[%d]=%p\n", proc->pid, ava_pyscidx, proc->pysc_pgmd[ava_pyscidx].pva, ava_pyscidx, a);
  	    proc->pysc_pgmd[ava_pyscidx].pva = (void*)a;
      }
    #endif
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  #ifndef SELECTION_NONE 
  int pyscidx; 
  #endif
  
  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a += (NPTENTRIES - 1) * PGSIZE;
    else if((*pte & PTE_P) != 0)
    {
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = p2v(pa);
      kfree(v);
      
      #ifndef SELECTION_NONE
        if (proc->pid > 2 && proc->pgdir == pgdir) 
        {
          if ((pyscidx = get_pgidx_in_pysc((void*)a)) == -1){
            panic("deallocate pysc: pyscidx not found");
          }
          //cprintf("pid=%d deallocate: pysc[%d]=%p --> swap[%d]=%p\n", proc->pid, pyscidx, proc->pysc_pgmd[pyscidx].pva, pyscidx, (void*)-1);
          clear_pysc_and_block_holls(pyscidx);
          //proc->pysc_pgmd[pyscidx].pva = (void*)-1;
        }
      #endif

      *pte = 0;
    }    
    #ifndef SELECTION_NONE
    //not present and swapped out
    else if (((*pte & PTE_P) == 0) && ((*pte & PTE_PG) != 0))
    { 
      if (proc->pid > 2 && proc->pgdir == pgdir) {
	      int swapidx;
	      if ((swapidx = get_pgidx_in_swap((void*)a)) == -1){
	        panic("deallocate swap: swapidx not found");
	      }
	      //cprintf("pid=%d deallocate: swap[%d]=%p --> swap[%d]=%p\n", proc->pid, swapidx, proc->swap_pgmd[swapidx].pva, swapidx, (void*)-1);
	      proc->swap_pgmd[swapidx].pva = (void*)-1;
	    }
		}
    #endif
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;
  
  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = p2v(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }

  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)p2v(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, v2p(mem), flags) < 0)
      goto bad;
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)p2v(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

#ifndef SELECTION_NONE

int
handle_pgfault(void* fadd)
{
  //index of page in swap file and in pysical memory
  int swap_pgidx;
  int pysc_pgidx;
  //cprintf("in page_fault, page that we want to read is %x\n", fadd);
  // the page doesn't exist in swap file or swap file doesn't allocated
  if ((swap_pgidx = get_pgidx_in_swap(fadd)) == -1 || proc->swapFile == 0)  {
    
    /*cprintf("fadd=%p\n", fadd);
    int i;
    for (i = 0; i < MAX_FILE_PAGES; i++)
    	 cprintf("pid=%d swap[%d]=%p\n", proc->pid, i, proc->swap_pgmd[i].pva);
  	for (i = 0; i < MAX_FILE_PAGES; i++)
    	 cprintf("pid=%d pysc[%d]=%p\n", proc->pid, i, proc->pysc_pgmd[i].pva);
    */

    panic("handle_pgfault: not in storage");

    return -1;
  }

  // ----- the page exist in swap file ----
  
  //return pointer to page on pysc memory
  pte_t* pte = walkpgdir(proc->pgdir, fadd, 0); 
  *pte = *pte & ~PTE_PG;  //not swaped out

  if ((pysc_pgidx = get_free_pgidx_pysc()) == -1){ 
    // pysc memory is full need to swap out a page
    pysc_pgidx = paged_out(swap_pgidx); //after that --> pysc_pgidx is free for future allocation
  }
  else {
    proc->swap_pgmd[swap_pgidx].pva = (void*)-1;
  } 

  char* mem;
  if ((mem = kalloc()) == 0) {
    panic("handle_pgfault: Kalloc - problem in allocate pysical memory"); 
    return -1;
  }
  
  uint pgoff = ((uint)swap_pgidx) * PGSIZE;

  // kalloc to buf
  memset(mem, 0, PGSIZE);   
  if (readFromSwapFile(proc, mem, pgoff, PGSIZE) == -1) {
    panic("handle_pgfault: read from swap file");
    return -1;
  }

  if (mappages(proc->pgdir, fadd, PGSIZE, v2p(mem), PTE_W|PTE_U) == -1){
    panic("handle_pgfault: mapppages - problem in mapping virtual to pysical");
    return -1;
  }
  
  //update pysc memory struct  
  proc->pysc_pgmd[pysc_pgidx].pva = fadd;
  proc->pgfault_num++;

  return 0;
}

#define SWAP_BUF_DIVS   (4)
#define SWAP_BUF_SIZE  PGSIZE / SWAP_BUF_DIVS

void
copy_swap_content(struct proc* dstp, struct proc* srcp)
{
  uint i, j, pgoff; //copy in piceas of bufsize
  char buf[SWAP_BUF_SIZE];

  for (i = 0; i < MAX_FILE_PAGES; i++) 
  {
    if (srcp->swap_pgmd[i].pva != (void*)-1) 
    {
      for (j = 0; j < SWAP_BUF_DIVS; j++)
      {
        pgoff = (i * PGSIZE) + (j * SWAP_BUF_SIZE);
        memset(buf, 0, SWAP_BUF_SIZE);
        readFromSwapFile(srcp, buf, pgoff, PGSIZE);
        writeToSwapFile(dstp, buf, pgoff, PGSIZE);
      }
    }
  }
}

//init all proc pages initial data inculing swp file
void 
create_proc_pgmd(struct proc* p)
{
 	createSwapFile(p);
  init_pysc_pgmd(p);
  init_swap_pgmd(p);
  p->oldest_pgidx = 0;
  p->pgfault_num = 0;
  p->pgout_num = 0;
}

// clear all proc pages data
void 
free_proc_pgmd(struct proc* p, uint remove)
{
  if (remove)
    removeSwapFile(p);
  //else
  //  p->swapFile = 0; //importent to handle this case carfully so we won't lose the swap pointer (backup proc befoecalling this function)

  init_pysc_pgmd(p);
  init_swap_pgmd(p);
  p->oldest_pgidx = 0;
  p->pgfault_num = 0;
  p->pgout_num = 0;
}

// 1. copy srcp swap file with the name of dstp   
// 2. copy all srcp pages meta data to dstp
void 
copy_proc_pgmd(struct proc* dstp, struct proc* srcp)
{
  createSwapFile(dstp);
  copy_swap_content(dstp, srcp);
  copy_pysc_pgmd(dstp, srcp);
  copy_swap_pgmd(dstp, srcp);
  dstp->oldest_pgidx = srcp->oldest_pgidx;
  dstp->pgout_num = srcp->pgout_num;
  dstp->pgfault_num = srcp->pgfault_num;
}

#endif
