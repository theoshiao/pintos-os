#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy, *fn_copy2, *saveptr;
  tid_t tid;

  sema_init (&temporary, 0);
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Make another copy for parsing name of executable */
  fn_copy2 = palloc_get_page (0);
  if (fn_copy2 == NULL)
    return TID_ERROR;
  strlcpy (fn_copy2, fn_copy, PGSIZE);

  /* Check if file exists. */
  char *fn_exec = strtok_r (fn_copy2, " ", &saveptr);

  if (!filesys_open (fn_exec)) {
    palloc_free_page (fn_copy);
    palloc_free_page (fn_copy2);
    return TID_ERROR;
  }

  /* Synch with load */
  struct load_synch load_info;
  load_info.filename = fn_copy;
  sema_init (&(load_info.sema), 0);
  load_info.success = true;
  load_info.parent_working_dir = thread_current ()->working_dir;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (fn_exec, PRI_DEFAULT, start_process, (void *)(&load_info));
  palloc_free_page (fn_copy2);

  /* wait for load to complete */
  sema_down (&(load_info.sema)); 

  if (tid == TID_ERROR || !load_info.success)
    return TID_ERROR;

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *load_info_)
{
  struct load_synch *load_info = (struct load_synch *) load_info_;
  char *file_name = load_info->filename;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* If load failed, set load info and quit. */
  palloc_free_page (file_name);
  if (!success)
    {
      load_info->success = false;
      sema_up (&(load_info->sema));
      thread_exit ();
    }

  if (load_info->parent_working_dir != NULL)
    thread_current ()->working_dir = dir_reopen (load_info->parent_working_dir);
  else
    thread_current ()->working_dir = dir_open_root ();

  load_info->success = true;
  sema_up (&(load_info->sema));

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED)
{
  /* find child list elem */
  struct thread *cur = thread_current ();
  struct wait_status *found_child = NULL;
  struct wait_status *child;
  struct list_elem *e;

  for (e = list_begin (&(cur->children)); e != list_end (&(cur->children));
       e = list_next (e))
    {
      child = list_entry (e, struct wait_status, wait_elem);

      if (((tid_t) child->pid) == child_tid)
        {
          found_child = child;
          break;
        }
    }

  if (found_child == NULL || found_child->parent_waited)
    return -1;

  found_child->parent_waited = true;
  sema_down (&(found_child->sema));
  return found_child->exit_code;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();

  /* handle exception exit */
  if (!(cur->own_wait_status->valid))
    {
      cur->own_wait_status->exit_code = -1;
      cur->own_wait_status->valid = true;
    }

  lock_acquire (&(cur->own_wait_status->lock));
  (cur->own_wait_status->ref_count)--;
  lock_release (&(cur->own_wait_status->lock));
  sema_up (&(cur->own_wait_status->sema));

  if (cur->own_wait_status->ref_count == 0)
    free (cur->own_wait_status);

  /* Close current working directory */
  if (cur->working_dir != NULL)
    dir_close (cur->working_dir);

  struct list_elem *e;
  struct wait_status *child;

  /* Keep track of children that need to be freed */
  struct wait_status *dead_children[list_size (&(cur->children))];
  int free_count = 0;

  for (e = list_begin (&(cur->children)); e != list_end (&(cur->children));
       e = list_next (e))
    {
      child = list_entry (e, struct wait_status, wait_elem);
      lock_acquire(&(child->lock));
      (child->ref_count)--;
      lock_release(&(child->lock));

      /* track children that need to be freed */
      if (child->ref_count == 0)
        {
          dead_children[free_count] = child;
          free_count++;
        }
    }

  /* free dead child wait statuses */
  int i;
  for (i = 0; i < free_count; i++)
    free (dead_children[i]);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  uint32_t *pd;
  pd = cur->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, char *exec_cmd);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name_, void (**eip) (void), void **esp)
{
  /* Make 2 non-constant copies of file_name_ for 
     tokenizing. First is mutated by call to strtok_r.
     Second is mutated in setup_stack */
  char *file_name = palloc_get_page (0);
  char *file_name2 = palloc_get_page (0);
  strlcpy (file_name, file_name_, PGSIZE);
  strlcpy (file_name2, file_name_, PGSIZE);

  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  char *saveptr;

  /* Open executable file. */
  file = filesys_open (strtok_r (file_name, " ", &saveptr));

  if (file == NULL)
    {
      printf ("load: %s: open failed\n", file_name);
      goto done;
    }

  add_fd (file);
  file_deny_write (file);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;

      file_seek (file, file_ofs);
      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;

      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, file_name2))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  palloc_free_page(file_name);
  palloc_free_page(file_name2);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, char *exec_cmd)
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }

  int token_count = 0;
  size_t token_len;
  char *save_ptr;
  uint8_t *byte_page_ptr = (uint8_t *) *esp;

  /* Count the number of tokens */
  char *exec_cmd_tokens = palloc_get_page (0);
  strlcpy (exec_cmd_tokens, exec_cmd, PGSIZE);

  char *token = strtok_r (exec_cmd_tokens, " ", &save_ptr);
  while (token != NULL)
    {
      token_count++;
      token = strtok_r (NULL, " ", &save_ptr);
    }
  palloc_free_page (exec_cmd_tokens);

  uint8_t *arg_addrs[token_count];

  /* Populate the stack */
  token_count = 0;
  save_ptr = NULL;

  /* Get first token, which we know must exist (filename) */
  token = strtok_r (exec_cmd, " ", &save_ptr);

  /* Copy token to page and get next. Keep token count */
  while (token != NULL)
    {
      token_len = strlen (token);
      /* add 1 for null terminator */
      byte_page_ptr -= (token_len + 1);
      
      strlcpy ((char *)byte_page_ptr, token, token_len + 1);
      arg_addrs[token_count] = byte_page_ptr;
      token_count++;
      token = strtok_r (NULL, " ", &save_ptr);
    }

  /* word align down ("& 3" to get "mod 4" operation) */
  byte_page_ptr -= ((int) byte_page_ptr) & 3;
  uint32_t *word_page_ptr = (uint32_t *) byte_page_ptr;
  word_page_ptr--;

  /* add 0 to satisfy arv[argc] == 0 */
  word_page_ptr[0] = (uint32_t) 0;

  /* populate argv[i] */
  int i;
  for (i = 0; i < token_count; i++)
    {
      word_page_ptr--;
      word_page_ptr[0] = (uint32_t) arg_addrs[token_count - i - 1];
    }

  /* argv */
  word_page_ptr--;
  word_page_ptr[0] = (uint32_t) (word_page_ptr + 1);

  /* argc */
  word_page_ptr--;
  word_page_ptr[0] = (uint32_t) token_count;

  /* fake return address */
  word_page_ptr--;
  word_page_ptr[0] = (uint32_t) 0; 
  
  *esp = (void *) word_page_ptr;
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
