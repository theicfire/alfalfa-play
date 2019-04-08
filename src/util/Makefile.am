AM_CPPFLAGS = $(CXX11_FLAGS)
AM_CXXFLAGS = $(PICKY_CXXFLAGS) $(NODEBUG_CXXFLAGS)

noinst_LIBRARIES = libalfalfautil.a

SUFFIXES = .asm
.asm.o:
	$(AS) $(ASFLAGS) -I$(srcdir)/../asm/ $<

libalfalfautil_a_SOURCES = chunk.hh exception.hh file.cc \
	file_descriptor.hh file.hh \
	optional.hh \
	mmap_region.hh mmap_region.cc \
	finally.hh
