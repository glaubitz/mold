#include "mold.h"

#include <limits>
#include <zlib.h>

static u64 read64be(u8 *buf) {
  return ((u64)buf[0] << 56) | ((u64)buf[1] << 48) |
         ((u64)buf[2] << 40) | ((u64)buf[3] << 32) |
         ((u64)buf[4] << 24) | ((u64)buf[5] << 16) |
         ((u64)buf[6] << 8)  | (u64)buf[7];
}

template <typename E>
void InputSection<E>::do_uncompress(Context<E> &ctx, std::string_view data,
                                    u64 size) {
  std::vector<u8> *buf = new std::vector<u8>(size);
  ctx.owning_bufs.push_back(std::unique_ptr<std::vector<u8>>(buf));

  unsigned long size2 = size;
  if (uncompress(buf->data(), &size2, (u8 *)&data[0], data.size()) != Z_OK)
    Fatal(ctx) << file << ": " << name << ": uncompress failed";
  if (size != size2)
    Fatal(ctx) << file << ": " << name << ": uncompress: invalid size";
  contents = {(char *)buf->data(), size};
}

// Uncompress old-style compressed section
template <typename E>
void InputSection<E>::uncompress_old_style(Context<E> &ctx) {
  std::string_view data = file.get_string(ctx, shdr);
  if (!data.starts_with("ZLIB") || data.size() <= 12)
    Fatal(ctx) << file << ": " << name << ": corrupted compressed section";
  u64 size = read64be((u8 *)&data[4]);
  do_uncompress(ctx, data.substr(12), size);
}

// Uncompress new-style compressed section
template <typename E>
void InputSection<E>::uncompress_new_style(Context<E> &ctx) {
  // New-style compressed section
  std::string_view data = file.get_string(ctx, shdr);
  if (data.size() < sizeof(ElfChdr<E>))
    Fatal(ctx) << file << ": " << name << ": corrupted compressed section";

  ElfChdr<E> &hdr = *(ElfChdr<E> *)&data[0];
  if (hdr.ch_type != ELFCOMPRESS_ZLIB)
    Fatal(ctx) << file << ": " << name << ": unsupported compression type";
  do_uncompress(ctx, data.substr(sizeof(ElfChdr<E>)), hdr.ch_size);
}

template <typename E>
void InputSection<E>::copy_buf(Context<E> &ctx) {
  if (shdr.sh_type == SHT_NOBITS || shdr.sh_size == 0)
    return;

  // Copy data
  u8 *base = ctx.buf + output_section->shdr.sh_offset + offset;
  memcpy(base, contents.data(), contents.size());

  // Apply relocations
  if (shdr.sh_flags & SHF_ALLOC)
    apply_reloc_alloc(ctx, base);
  else
    apply_reloc_nonalloc(ctx, base);
}

template <typename E>
static i64 get_output_type(Context<E> &ctx) {
  if (ctx.arg.shared)
    return 0;
  if (ctx.arg.pie)
    return 1;
  return 2;
}

template <typename E>
static i64 get_sym_type(Context<E> &ctx, Symbol<E> &sym) {
  if (sym.is_absolute(ctx))
    return 0;
  if (!sym.is_imported)
    return 1;
  if (sym.get_type() != STT_FUNC)
    return 2;
  return 3;
}

template <typename E>
void InputSection<E>::dispatch(Context<E> &ctx, Action table[3][4],
                               u16 rel_type, i64 i) {
  std::span<ElfRel<E>> rels = get_rels(ctx);
  const ElfRel<E> &rel = rels[i];
  Symbol<E> &sym = *file.symbols[rel.r_sym];
  bool is_readonly = !(shdr.sh_flags & SHF_WRITE);
  Action action = table[get_output_type(ctx)][get_sym_type(ctx, sym)];

  switch (action) {
  case NONE:
    rel_types[i] = rel_type;
    return;
  case ERROR:
    break;
  case COPYREL:
    if (!ctx.arg.z_copyreloc)
      break;
    if (sym.esym().st_visibility == STV_PROTECTED)
      Error(ctx) << *this << ": cannot make copy relocation for "
                 << " protected symbol '" << sym << "', defined in "
                 << *sym.file;
    sym.flags |= NEEDS_COPYREL;
    rel_types[i] = rel_type;
    return;
  case PLT:
    sym.flags |= NEEDS_PLT;
    rel_types[i] = rel_type;
    return;
  case DYNREL:
    if (is_readonly)
      break;
    sym.flags |= NEEDS_DYNSYM;
    rel_types[i] = R_DYN;
    file.num_dynrel++;
    return;
  case BASEREL:
    if (is_readonly)
      break;
    rel_types[i] = R_BASEREL;
    file.num_dynrel++;
    return;
  default:
    unreachable(ctx);
  }

  Error(ctx) << *this << ": " << rel_to_string<E>(rel.r_type)
             << " relocation against symbol `" << sym
             << "' can not be used; recompile with -fPIE";
}

template class InputSection<X86_64>;
template class InputSection<I386>;
