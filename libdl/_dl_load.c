#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <linux/elf.h>

#include "_dl_int.h"

#define _ELF_DWN_ROUND(ps,n)	((n)&(~((ps)-1)))
#define _ELF_UP_ROUND(ps,n)	((((n)&((ps)-1))?(ps):0)+ _ELF_DWN_ROUND((ps),(n)))
#define _ELF_RST_ROUND(ps,n)	((n)&((ps)-1))

/* this is an arch specific "return jump" for the relocation */
void _dl_jump();

/*
 * this file is a Q. & D. hack ... don't think this is bug free or meaningfull
 */

static void *do_map_in(void *base, unsigned long length, int flags, int fd, unsigned long offset)
{
  int perm = 0;
  if (flags & PF_X) perm|=PROT_EXEC;
  if (flags & PF_R) perm|=PROT_READ;
  if (flags & PF_W) perm|=PROT_WRITE;
  return mmap(base, length, perm, MAP_PRIVATE|((base)?MAP_FIXED:0), fd, offset);
}

static struct _dl_handle *_dl_map_lib(const char*fn, const char*pathname, int fd, int flags)
{
  struct _dl_handle* ret=0;
  int ps=getpagesize();
  int i;
  unsigned char buf[1024];
  char *m=0,*d;

  unsigned long l;
  struct stat st;

  Elf32_Ehdr *eh;
  Elf32_Phdr *ph;

  int ld_nr=0;
  Elf32_Phdr *ld[4]={0,0,0,0};
  Elf32_Phdr *dyn=0;

  if (fd==-1) return 0;

#ifdef DEBUG
  printf("_dl_load: %s\n",pathname);
#endif

  if (fstat(fd,&st)<0) {
    close(fd);
    _dl_error=1;
    return 0;
  }
  else {
    // use st_dev and st_ino for identification
  }

  read(fd, buf, 1024);
  eh=(Elf32_Ehdr*)buf;
  ph=(Elf32_Phdr*)&buf[eh->e_phoff];

  for (i=0; i<eh->e_phnum; i++) {
    if (ph[i].p_type==PT_LOAD) {
      ld[ld_nr++]=ph+i;
    }
    if (ph[i].p_type==PT_DYNAMIC) {
      dyn=ph+i;
    }
  }

  /* get a little space for *.so administration */
//  dl_test = _dl_get_handle();

  if (ld_nr==1) {
    unsigned long offset = _ELF_DWN_ROUND(ps,ld[0]->p_offset);
    unsigned long off = _ELF_RST_ROUND(ps,ld[0]->p_offset);
    unsigned long length = _ELF_UP_ROUND(ps,ld[0]->p_memsz+off);
    ret = _dl_get_handle();

    m = (char*)do_map_in(0, length, ld[0]->p_flags, fd, offset);

    /* zero pad bss */
    l = ld[0]->p_offset+ld[0]->p_filesz;
    memset(m+l,0,length-l);

    ret->mem_base=m;
    ret->mem_size=length;
  }
  else if (ld_nr==2) { /* aem... yes Quick & Really Dirty / for the avarage 99% */
    unsigned long text_addr = _ELF_DWN_ROUND(ps,ld[0]->p_vaddr);
    unsigned long text_offset = _ELF_DWN_ROUND(ps,ld[0]->p_offset);
    unsigned long text_off = _ELF_RST_ROUND(ps,ld[0]->p_offset);
    unsigned long text_size = _ELF_UP_ROUND(ps,ld[0]->p_memsz+text_off);

    unsigned long data_addr = _ELF_DWN_ROUND(ps,ld[1]->p_vaddr);
    unsigned long data_offset = _ELF_DWN_ROUND(ps,ld[1]->p_offset);
    unsigned long data_off = _ELF_RST_ROUND(ps,ld[1]->p_offset);
    unsigned long data_size = _ELF_UP_ROUND(ps,ld[1]->p_memsz+data_off);
    unsigned long data_fsize = _ELF_UP_ROUND(ps,ld[1]->p_filesz+data_off);

    ret = _dl_get_handle();
    /* mmap all mem_blocks for *.so */
    m = (char*) do_map_in(0,text_size+data_size,ld[0]->p_flags,fd,text_offset);

    /* release data,bss part */
    mprotect(m+data_addr, data_size, PROT_NONE);

    /* mmap data,bss part */
    d = (char*) do_map_in(m+data_addr,data_fsize,ld[1]->p_flags,fd,data_offset);

    /* zero pad bss */
    l = data_off+ld[1]->p_filesz;
    memset(d+l,0,data_size-l);

    /* more bss ? */
    if (data_size>data_fsize) {
      l = data_size-data_fsize;
      mmap(d+data_fsize, l, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
    }

    ret->mem_size=text_size+data_size;
    ret->mem_base=m;
    ret->img_off = text_addr;
  }
  close(fd);

  if (ret) {
    ret->lnk_count=0;
    ret->name=strdup(fn);
    ret->dyn_str_tab=(char*)m+dyn->p_vaddr;
  }
  return ret;
}


struct _dl_handle* _dl_dyn_scan(struct _dl_handle* dh, void* dyn_addr, int flags)
{
  Elf32_Dyn* dyn_tab = dyn_addr;
  void (*init)()=0;
  unsigned long* got=0;
  void* jmprel=0;
  int pltreltype=0;
  int pltrelsize=0;
  int rel=0;
  int relent=0;
  int relsize=0;

  int i;

#ifdef DEBUG
  printf("_dl_load pre resolv %08x\n",dyn_tab);
#endif
  dh->dyn_str_tab = 0;
  dh->flag_global = flags&RTLD_GLOBAL;

#ifdef DEBUG
  printf("_dl_load IN resolv\n");
#endif
  for(i=0;dyn_tab[i].d_tag;i++) {
#ifdef DEBUG
//    printf("_dl_load dyn %d, %08lx\n",dyn_tab[i].d_tag, dyn_tab[i].d_un.d_val);
#endif
    if (dyn_tab[i].d_tag==DT_HASH) {
      dh->hash_tab = (unsigned long*)(dh->mem_base+dyn_tab[i].d_un.d_ptr);
#ifdef DEBUG
      printf("_dl_load have hash @ %08lx\n",(long)dh->hash_tab);
#endif
    }
    if (dyn_tab[i].d_tag==DT_SYMTAB) {
      dh->dyn_sym_tab = (Elf32_Sym*)(dh->mem_base+dyn_tab[i].d_un.d_ptr);
#ifdef DEBUG
      printf("_dl_load have dyn_sym_tab @ %08lx\n",(long)dh->dyn_sym_tab);
#endif
    }
    if (dyn_tab[i].d_tag==DT_STRTAB) {
      dh->dyn_str_tab = (char*)(dh->mem_base+dyn_tab[i].d_un.d_ptr);
#ifdef DEBUG
      printf("_dl_load have dyn_str_tab @ %08lx\n",(long)dh->dyn_str_tab);
#endif
    }

    /* INIT / FINI */
    if (dyn_tab[i].d_tag==DT_FINI) {
      dh->fini = (void(*)(void))(dh->mem_base+dyn_tab[i].d_un.d_val);
#ifdef DEBUG
      printf("_dl_load have fini @ %08lx\n",(long)dh->fini);
#endif
    }
    if (dyn_tab[i].d_tag==DT_INIT) {
      init = (void(*)(void))(dh->mem_base+dyn_tab[i].d_un.d_val);
#ifdef DEBUG
      printf("_dl_load have init @ %08lx\n",(long)init);
#endif
    }

    /* PLT / Relocation entries for PLT in GOT */
    if (dyn_tab[i].d_tag==DT_PLTGOT) {
      got=(unsigned long*)(dh->mem_base+dyn_tab[i].d_un.d_val);
      dh->pltgot=got;
#ifdef DEBUG
      printf("_dl_load have plt got @ %08lx\n",(long)got);
#endif
    }
    if (dyn_tab[i].d_tag==DT_PLTREL) {
      pltreltype=dyn_tab[i].d_un.d_val;
#ifdef DEBUG
      printf("_dl_load have pltreltype @ %08lx\n",(long)pltreltype);
#endif
    }
    if (dyn_tab[i].d_tag==DT_PLTRELSZ) {
      pltrelsize=dyn_tab[i].d_un.d_val;
#ifdef DEBUG
      printf("_dl_load have pltrelsize @ %08lx\n",(long)pltrelsize);
#endif
    }
    if (dyn_tab[i].d_tag==DT_JMPREL) {
      jmprel=(dh->mem_base+dyn_tab[i].d_un.d_val);
      dh->plt_rel=jmprel;
#ifdef DEBUG
      printf("_dl_load have jmprel @ %08lx\n",(long)jmprel);
#endif
    }

    /* Relocation */
    if (dyn_tab[i].d_tag==DT_REL) {
      rel=dyn_tab[i].d_un.d_val;
#ifdef DEBUG
      printf("_dl_load have rel @ %08lx\n",(long)rel);
#endif
    }
    if (dyn_tab[i].d_tag==DT_RELENT) {
      relent=dyn_tab[i].d_un.d_val;
#ifdef DEBUG
      printf("_dl_load have relent  @ %08lx\n",(long)relent);
#endif
    }
    if (dyn_tab[i].d_tag==DT_RELSZ) {
      relsize=dyn_tab[i].d_un.d_val;
#ifdef DEBUG
      printf("_dl_load have relsize @ %08lx\n",(long)relsize);
#endif
    }

    if (dyn_tab[i].d_tag==DT_TEXTREL) {
      printf("_dl_load have textrel ?!? -> SUE LIB CREATOR ! \n");
      _dl_free_handle(dh);
      _dl_error = 2;
      return 0;
    }
  }
#ifdef DEBUG
  printf("_dl_load post dynamic scan %08x\n",dh);
#endif

  if ((got=_dlsym(dh,"_GLOBAL_OFFSET_TABLE_"))) {
    dh->got=got;
#ifdef DEBUG
    printf("_dl_load found a GOT @ %08lx\n",(long)got);
#endif
    /* GOT */
    got[0]+=(unsigned long)dh->mem_base;	/* reloc dynamic pointer */
    got[1] =(unsigned long)dh;
    got[2] =(unsigned long)(_dl_jump);	/* sysdep jump to _dl_rel */
    /* */
  }
  else {
    printf("_dl_load non PIC dynamic -> SUE USER ! \n");
    if (dh) _dl_free_handle(dh);
    _dl_error = 2;
    return 0;
  }

  if (rel) {
#ifdef DEBUG
    printf("_dl_load try to relocate some values \n");
#endif
    _dl_relocate(dh,(Elf32_Rel*)rel,relsize/relent);
  }

  // load other libs
  for(i=0;dyn_tab[i].d_tag;i++) {
    if (dyn_tab[i].d_tag==DT_NEEDED) {
      char *lib_name=dh->dyn_str_tab+dyn_tab[i].d_un.d_val;
#ifdef DEBUG
      printf("_dl_load needed for this lib: %s\n",lib_name);
#endif
      //_dl_open(lib_name,flags);
      _dl_queue_lib(lib_name,flags);
    }
  }

  _dl_open_dep();

  /* do PTL / GOT relocation */
  if (pltreltype == DT_REL) {
    Elf32_Rel *tmp = jmprel;
#ifdef DEBUG
    printf("_dl_load: rel got\n");
#endif
    for (;(char*)tmp<(((char*)jmprel)+pltrelsize);(char*)tmp=((char*)tmp)+sizeof(Elf32_Rel)) {
      if ((flags&RTLD_NOW)) {
	unsigned long sym=(unsigned long)_dl_sym(dh,ELF32_R_SYM(tmp->r_info));
	if (sym) *((unsigned long*)(dh->mem_base+tmp->r_offset))=sym;
	else {
	  _dl_free_handle(dh);
	  _dl_error = 3;
	  return 0;
	}
      }
      else
	*((unsigned long*)(dh->mem_base+tmp->r_offset))+=(unsigned long)dh->mem_base;
#ifdef DEBUG
      printf("_dl_load rel @ %08lx with type %d -> %d\n",(long)dh->mem_base+tmp->r_offset,ELF32_R_TYPE(tmp->r_info),ELF32_R_SYM(tmp->r_info));
      printf("_dl_load -> %08lx\n",*((unsigned long*)(dh->mem_base+tmp->r_offset)));
#endif
    }
  }
  if (pltreltype == DT_RELA) {
    Elf32_Rela *tmp = jmprel;
#ifdef DEBUG
    printf("_dl_load: rela got\n");
#endif
    for (;(char*)tmp<(((char*)jmprel)+pltrelsize);(char*)tmp=((char*)tmp)+sizeof(Elf32_Rel)) {
      if ((flags&RTLD_NOW)) {
	unsigned long sym=(unsigned long)_dl_sym(dh,ELF32_R_SYM(tmp->r_info));
	if (sym) *((unsigned long*)(dh->mem_base+tmp->r_offset))=sym;
	else {
	  _dl_free_handle(dh);
	  _dl_error = 3;
	  return 0;
	}
      }
      else
	*((unsigned long*)(dh->mem_base+tmp->r_offset))=(unsigned long)(dh->mem_base+tmp->r_addend);
#ifdef DEBUG
      printf("_dl_load rela @ %08lx with type %d -> %d\n",(long)dh->mem_base+tmp->r_offset,ELF32_R_TYPE(tmp->r_info),ELF32_R_SYM(tmp->r_info));
      printf("_dl_load -> %08lx\n",*((unsigned long*)(dh->mem_base+tmp->r_offset)));
#endif
    }
  }

  // _dl_load depending libs ...
#ifdef DEBUG
  printf("_dl_load post resolv, pre init\n");
#endif
  if (init) init();
#ifdef DEBUG
  printf("_dl_load post resolv, post init\n");
#endif

  return dh;
}

void *_dl_load(const char*fn, const char*pathname, int fd, int flags)
{
  struct _dl_handle* ret=0;
  if ((ret=_dl_map_lib(fn,pathname,fd,flags))) {
    ret=_dl_dyn_scan(ret,(void*)(ret->dyn_str_tab),flags);
  }
  return ret;
}