ifeq (,$(wildcard ../config.mak))
$(error "../config.mak is not present, run configure !")
endif
include ../config.mak

LIBNAME = libvalhalla
STATIC_LIBNAME = $(LIBNAME).a
SHARED_LIBNAME = $(LIBNAME).so
SHARED_LIBNAME_VERSION = $(SHARED_LIBNAME).$(VERSION)
SHARED_LIBNAME_MAJOR = $(SHARED_LIBNAME).$(shell echo $(VERSION) | cut -f1 -d.)
SHARED_LIBNAME_FLAGS = -shared -Wl,-soname,$(SHARED_LIBNAME_MAJOR)

ifeq ($(BUILD_STATIC),yes)
  BUILD_RULES += lib_static
endif
ifeq ($(BUILD_SHARED),yes)
  BUILD_RULES += lib_shared
  ifeq ($(BUILD_DYLIB),yes)
    SHARED_LIBNAME         = $(LIBNAME).dylib
    SHARED_LIBNAME_VERSION = $(LIBNAME).$(VERSION).dylib
    SHARED_LIBNAME_MAJOR   = $(LIBNAME).$(shell echo $(VERSION) | cut -f1 -d.).dylib
    SHARED_LIBNAME_FLAGS   = -dynamiclib -Wl,-headerpad_max_install_names,-undefined,dynamic_lookup,-install_name,$(SHARED_LIBNAME_VERSION)
  else
    ifeq ($(BUILD_MINGW32),yes)
      SHARED_LIBNAME         = $(LIBNAME)-$(shell echo $(VERSION) | cut -f1 -d.).dll
      SHARED_LIBNAME_VERSION = $(SHARED_LIBNAME)
      SHARED_LIBNAME_MAJOR   = $(SHARED_LIBNAME)
      SHARED_LIBNAME_FLAGS   = -shared -Wl,--out-implib=$(LIBNAME).dll.a -Wl,--export-all-symbols -Wl,--enable-auto-import
    endif
  endif
endif

SRCS =  database.c \
	dbmanager.c \
	dispatcher.c \
	event_handler.c \
	fifo_queue.c \
	lavf_utils.c \
	list.c \
	logs.c \
	md5.c \
	metadata.c \
	ondemand.c \
	osdep.c \
	parser.c \
	scanner.c \
	stats.c \
	thread_utils.c \
	timer_thread.c \
	utils.c \
	valhalla.c \

EXTRADIST = \
	database.h \
	dbmanager.h \
	dispatcher.h \
	downloader.h \
	event_handler.h \
	fifo_queue.h \
	grabber.h \
	grabber_allocine.h \
	grabber_amazon.h \
	grabber_chartlyrics.h \
	grabber_common.h \
	grabber_dummy.h \
	grabber_exif.h \
	grabber_ffmpeg.h \
	grabber_imdb.h \
	grabber_lastfm.h \
	grabber_local.h \
	grabber_lyricwiki.h \
	grabber_nfo.h \
	grabber_tmdb.h \
	grabber_tvdb.h \
	grabber_tvrage.h \
	grabber_utils.h \
	hmac_sha256.h \
	lavf_utils.h \
	list.h \
	logs.h \
	md5.h \
	metadata.h \
	ondemand.h \
	osdep.h \
	parser.h \
	scanner.h \
	sha.h \
	sql_statements.h \
	stats.h \
	thread_utils.h \
	timer_thread.h \
	url_utils.h \
	utils.h \
	valhalla.h \
	valhalla_internals.h \
	xml_utils.h \

SRCS_GRABBER-$(GRABBER)			+= downloader.c \
					   grabber.c \
					   grabber_utils.c \
					   url_utils.c \

SRCS_GRABBER-$(XML)			+= xml_utils.c
SRCS_GRABBER-$(HMAC_SHA256)		+= hmac_sha256.c \
					   sha.c
SRCS_GRABBER-$(GRABBER_DUMMY)		+= grabber_dummy.c
SRCS_GRABBER-$(GRABBER_ALLOCINE)	+= grabber_allocine.c
SRCS_GRABBER-$(GRABBER_AMAZON)		+= grabber_amazon.c
SRCS_GRABBER-$(GRABBER_CHARTLYRICS)	+= grabber_chartlyrics.c
SRCS_GRABBER-$(GRABBER_EXIF)		+= grabber_exif.c
SRCS_GRABBER-$(GRABBER_FFMPEG)		+= grabber_ffmpeg.c
SRCS_GRABBER-$(GRABBER_IMDB)		+= grabber_imdb.c
SRCS_GRABBER-$(GRABBER_LASTFM)		+= grabber_lastfm.c
SRCS_GRABBER-$(GRABBER_LOCAL)		+= grabber_local.c
SRCS_GRABBER-$(GRABBER_LYRICWIKI)	+= grabber_lyricwiki.c
SRCS_GRABBER-$(GRABBER_NFO)		+= grabber_nfo.c
SRCS_GRABBER-$(GRABBER_TMDB)		+= grabber_tmdb.c
SRCS_GRABBER-$(GRABBER_TVDB)		+= grabber_tvdb.c
SRCS_GRABBER-$(GRABBER_TVRAGE)		+= grabber_tvrage.c

SRCS += $(SRCS_GRABBER-yes)

OBJS = $(SRCS:.c=.o)

.SUFFIXES: .c .o

all: depend $(BUILD_RULES)

.c.o:
	$(CC) -c $(OPTFLAGS) $(CFLAGS) $(CFG_CPPFLAGS) $(CPPFLAGS) -o $@ $<

lib_static: $(STATIC_LIBNAME)

lib_shared: $(SHARED_LIBNAME)

$(STATIC_LIBNAME): $(OBJS)
	$(AR) r $(STATIC_LIBNAME) $(OBJS)
	$(RANLIB) $(STATIC_LIBNAME)

$(SHARED_LIBNAME): $(OBJS)
	$(CC) $(SHARED_LIBNAME_FLAGS) \
	  $(OBJS) $(CFG_LDFLAGS) $(EXTRALIBS) $(LDFLAGS) -o $(SHARED_LIBNAME_VERSION)
	if [ $(BUILD_MINGW32) = no ]; then \
	  $(LN) -sf $(SHARED_LIBNAME_VERSION) $(SHARED_LIBNAME_MAJOR); \
	  $(LN) -sf $(SHARED_LIBNAME_MAJOR) $(SHARED_LIBNAME); \
	fi

clean:
	rm -f *.o *.a *.so* *.dll
	rm -f .depend

install: $(BUILD_RULES)
	$(INSTALL) -d $(libdir)
	[ $(BUILD_STATIC) = yes ] && $(INSTALL) -c $(STATIC_LIBNAME) $(libdir); \
	if [ $(BUILD_SHARED) = yes ]; then \
	  if [ $(BUILD_MINGW32) = no ]; then \
	    $(INSTALL) -c $(SHARED_LIBNAME_VERSION) $(libdir); \
	    $(LN) -sf $(SHARED_LIBNAME_VERSION) $(libdir)/$(SHARED_LIBNAME_MAJOR); \
	    $(LN) -sf $(SHARED_LIBNAME_MAJOR) $(libdir)/$(SHARED_LIBNAME); \
	  else \
	    $(INSTALL) -c $(SHARED_LIBNAME_VERSION) $(bindir); \
	    $(INSTALL) -c $(LIBNAME).dll.a $(libdir); \
	  fi \
	fi
	$(INSTALL) -d $(includedir)
	$(INSTALL) -c -m 644 valhalla.h $(includedir)

uninstall:
	if [ $(BUILD_MINGW32) = yes ]; then \
	  rm -f $(bindir)/$(SHARED_LIBNAME); \
	  rm -f $(libdir)/$(LIBNAME).dll.a; \
	fi
	rm -f $(libdir)/$(STATIC_LIBNAME)
	rm -f $(libdir)/$(SHARED_LIBNAME)*
	rm -f $(includedir)/valhalla.h

depend:
	$(CC) -MM $(CFLAGS) $(CFG_CPPFLAGS) $(CPPFLAGS) $(SRCS) 1>.depend

.PHONY: clean depend
.PHONY: uninstall

dist-all:
	cp $(EXTRADIST) $(SRCS) $(SRCS_GRABBER-no) Makefile $(DIST)

.PHONY: dist-all

#
# include dependency files if they exist
#
ifneq ($(wildcard .depend),)
include .depend
endif
