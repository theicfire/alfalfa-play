AM_CPPFLAGS = -I$(srcdir)/../util -I$(srcdir)/../decoder -I$(srcdir)/../display -I$(srcdir)/../input -I$(srcdir)/../encoder -I$(srcdir)/../net -I$(srcdir)/../../../fecpp $(X264_CFLAGS) $(CXX11_FLAGS)
AM_CXXFLAGS = $(PICKY_CXXFLAGS) $(NODEBUG_CXXFLAGS)
AM_LDFLAGS = $(STATIC_BUILD_FLAG)
BASE_LDADD = ../util/libalfalfautil.a $(X264_LIBS)

VP8PLAY_BUILD :=
if BUILDVP8PLAY
VP8PLAY_BUILD += salsify-receiver
endif

bin_PROGRAMS = salsify-sender $(VP8PLAY_BUILD)

salsify_sender_SOURCES = salsify-sender.cc
salsify_sender_LDADD = ../net/libnet.a ../../../fecpp/libfecpp.a $(BASE_LDADD)
salsify_sender_LDFLAGS = -pthread

salsify_receiver_SOURCES = salsify-receiver.cc
salsify_receiver_CPPFLAGS = $(AM_CPPFLAGS) $(GLU_CFLAGS) $(GLEW_CFLAGS) $(GLFW3_CFLAGS)
salsify_receiver_LDADD = ../net/libnet.a $(BASE_LDADD) $(GLU_LIBS) $(GLEW_LIBS) $(GLFW3_LIBS)
salsify_receiver_LDFLAGS = -pthread
