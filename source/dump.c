#include "ps4.h"
#include "dump.h"
#include "elf64.h"

#include "defines.h"

#define TRUE 1
#define FALSE 0

typedef struct {
    int index;
    uint64_t fileoff;
    size_t bufsz;
    size_t filesz;
    int enc;
} SegmentBufInfo;

void hexdump(void* input, int length, int relative)
{
  uint8_t* buffer = (uint8_t*)input;
  int i;

  for (i = 0; i < length; i++)
  {
    if ((i % 16) == 0)
    {
      printfsocket("%llx :", !relative ? (uint64_t)&buffer[i] : (uint64_t)i);
    }

    printfsocket(" %02x", buffer[i]);

    if (i > 0 && (i % 16) == 15)
    {
      printfsocket("\n");
    }
  }

  if ((i % 16) != 0)
  {
    printfsocket("\n");
  }

  //printfsocket("\n");
}

void print_phdr(Elf64_Phdr *phdr) {
    printfsocket("=================================\n");
    printfsocket("     p_type %08x\n", phdr->p_type);
    printfsocket("     p_flags %08x\n", phdr->p_flags);
    printfsocket("     p_offset %016llx\n", phdr->p_offset);
    printfsocket("     p_vaddr %016llx\n", phdr->p_vaddr);
    printfsocket("     p_paddr %016llx\n", phdr->p_paddr);
    printfsocket("     p_filesz %016llx\n", phdr->p_filesz);
    printfsocket("     p_memsz %016llx\n", phdr->p_memsz);
    printfsocket("     p_align %016llx\n", phdr->p_align);
}

int read_decrypt_segment(int fd, uint64_t index, uint64_t offset, size_t size, uint8_t *out) {
    uint64_t realOffset = (index << 32) | offset;
    uint8_t *addr = (uint8_t*)mmap(0, size, PROT_READ, MAP_PRIVATE | 0x80000, fd, realOffset);
    if (addr != MAP_FAILED) {
        memcpy(out, addr, size);
        munmap(addr, size);
        return TRUE;
    }
    else {
        printfsocket("mmap segment [%d] err(%d) : %s\n", index, errno, strerror(errno));
        return FALSE;
    }
}

int is_segment_in_other_segment(Elf64_Phdr *phdr, int index, Elf64_Phdr *phdrs, int num) {
    for (int i = 0; i < num; i += 1) {
        Elf64_Phdr *p = &phdrs[i];
        if (i != index) {
            if (p->p_filesz > 0) {
                // printfsocket("offset : %016x,  toffset : %016x\n", phdr->p_offset, p->p_offset);
                // printfsocket("offset : %016x,  toffset + size : %016x\n", phdr->p_offset, p->p_offset + p->p_filesz);
                if ((phdr->p_offset >= p->p_offset) && ((phdr->p_offset + phdr->p_filesz) <= (p->p_offset + p->p_filesz))) {
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}


SegmentBufInfo *parse_phdr(Elf64_Phdr *phdrs, int num, int *segBufNum) {
    // printfsocket("segment num : %d\n", num);
    SegmentBufInfo *infos = (SegmentBufInfo *)malloc(sizeof(SegmentBufInfo) * num);
    int segindex = 0;
    for (int i = 0; i < num; i += 1) {
        Elf64_Phdr *phdr = &phdrs[i];
        // print_phdr(phdr);

        if (phdr->p_filesz > 0) {
            if ((!is_segment_in_other_segment(phdr, i, phdrs, num)) || (phdr->p_type == 0x6fffff01)) {
                SegmentBufInfo *info = &infos[segindex];
                segindex += 1;
                info->index = i;
                info->bufsz = (phdr->p_filesz + (phdr->p_align - 1)) & (~(phdr->p_align - 1));
                info->filesz = phdr->p_filesz;
                info->fileoff = phdr->p_offset;
                info->enc = (phdr->p_type != 0x6fffff01) ? TRUE : FALSE;

                // printfsocket("seg buf info %d -->\n", segindex);
                // printfsocket("    index : %d\n    bufsz : 0x%016llX\n", info->index, info->bufsz);
                // printfsocket("    filesz : 0x%016llX\n    fileoff : 0x%016llX\n", info->filesz, info->fileoff);
            }
        }
    }
    *segBufNum = segindex;
    return infos;
}

void do_dump(char *saveFile, int fd, SegmentBufInfo *segBufs, int segBufNum, Elf64_Ehdr *ehdr) {
    FILE *sf = fopen(saveFile, "wb");
    if (sf != NULL) {
        size_t elfsz = 0x40 + ehdr->e_phnum * sizeof(Elf64_Phdr);
        printfsocket("elf header + phdr size : 0x%08X\n", elfsz);
        fwrite(ehdr, elfsz, 1, sf);

        for (int i = 0; i < segBufNum; i += 1) {
            printfsocket("sbuf index : %d, offset : 0x%016x, bufsz : 0x%016x, filesz : 0x%016x, enc : %d\n", segBufs[i].index, segBufs[i].fileoff, segBufs[i].bufsz, segBufs[i].filesz, segBufs[i].enc);
            uint8_t *buf = (uint8_t*)malloc(segBufs[i].bufsz);
            memset(buf, 0, segBufs[i].bufsz);
            if (segBufs[i].enc)
            {
                if (read_decrypt_segment(fd, segBufs[i].index, 0, segBufs[i].filesz, buf)) {
                    fseek(sf, segBufs[i].fileoff, SEEK_SET);
                    fwrite(buf, segBufs[i].bufsz, 1, sf);
                }
            }
            else
            {
                fseek(sf, segBufs[i].fileoff, SEEK_SET);
                fwrite(buf, segBufs[i].filesz, 1, sf);
            }
            free(buf);
        }
        fclose(sf);
    }
    else {
        printfsocket("fopen %s err : %s\n", saveFile, strerror(errno));
    }
}

void decrypt_and_dump_self(char *selfFile, char *saveFile) {
    int fd = open(selfFile, O_RDONLY, 0);
    if (fd != -1) {
        void *addr = mmap(0, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (addr != MAP_FAILED) {
            // printfsocket("mmap %s : %p\n", selfFile, addr);

            uint16_t snum = *(uint16_t*)((uint8_t*)addr + 0x18);
            Elf64_Ehdr *ehdr = (Elf64_Ehdr *)((uint8_t*)addr + 0x20 + snum * 0x20);
            // printfsocket("ehdr : %p\n", ehdr);

            // shdr fix
            ehdr->e_shoff = ehdr->e_shentsize = ehdr->e_shnum = ehdr->e_shstrndx = 0;

            Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)ehdr + 0x40);
            // printfsocket("phdrs : %p\n", phdrs);

            int segBufNum = 0;
            SegmentBufInfo *segBufs = parse_phdr(phdrs, ehdr->e_phnum, &segBufNum);
            do_dump(saveFile, fd, segBufs, segBufNum, ehdr);
            // printfsocket("dump completed\n");

            free(segBufs);
            munmap(addr, 0x4000);
        }
        else {
            printfsocket("mmap file %s err : %s\n", selfFile, strerror(errno));
        }
    }
    else {
        printfsocket("open %s err : %s\n", selfFile, strerror(errno));
    }
}
