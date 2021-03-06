AC_PLUGIN_DIR=codecs/audio
prefix=/usr/local/lib/ptlib
target_os=linux-gnu

SONAME    = speex

SRCDIR    = ./src/libspeex
OBJDIR	  = ./obj
PLUGINDIR = ../..

CC		=gcc
CXX		=g++
LDSO		=-shared -Wl,-soname,$(SONAME)
PLUGINEXT	=so
CFLAGS	       +=-g  -Os
STDCCFLAGS	=-fPIC -D__STDC_CONSTANT_MACROS -D USE_KISS_FFT=1 -D EXPORT= -D FIXED_POINT=1
LDFLAGS	       +=
EXTRACFLAGS     =-Isrc/include -Isrc/include/speex

SRCS 		=speexcodec.cxx \
bits.c buffer.c cb_search.c exc_10_16_table.c exc_10_32_table.c exc_20_32_table.c exc_5_256_table.c exc_5_64_table.c exc_8_128_table.c fftwrap.c filterbank.c filters.c gain_table.c gain_table_lbr.c hexc_10_32_table.c hexc_table.c high_lsp_tables.c jitter.c kiss_fft.c kiss_fftr.c lpc.c lsp.c lsp_tables_nb.c ltp.c mdf.c modes.c modes_wb.c nb_celp.c preprocess.c quant_lsp.c resample.c sb_celp.c scal.c smallft.c speex.c speex_callbacks.c speex_header.c stereo.c vbr.c vq.c window.c

vpath	%.o $(OBJDIR)
vpath	%.c $(SRCDIR)
vpath	%.cxx .

ifeq ($(VERBOSE),)
Q_CC = @echo [CC] `echo $< | sed s^@OPALDIR@/^^` ;
Q_LD = @echo [LD] `echo $(PLUGIN) | sed s^@OPALDIR@/^^` ;
endif


$(OBJDIR)/%.o : %.c
	@mkdir -p $(OBJDIR) >/dev/null 2>&1
	$(Q_CC)$(CC) -I../../../include $(STDCCFLAGS) $(OPTCCFLAGS) $(EXTRACFLAGS) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o : %.cxx
	@mkdir -p $(OBJDIR) >/dev/null 2>&1
	$(CXX) $(EXTRACCFLAGS) $(STDCCFLAGS) $(OPTCCFLAGS) $(CFLAGS) -c $< -o $@

PLUGIN	= ./$(SONAME)_audio_pwplugin.$(PLUGINEXT)

OBJECTS = $(addprefix $(OBJDIR)/,$(patsubst %.c,%.o,$(patsubst %.cxx,%.o,$(notdir $(SRCS)))))


all: $(PLUGIN)

$(PLUGIN): $(OBJECTS)
ifeq (solaris,$(findstring solaris,$(target_os)))

	$(Q_LD)$(CXX) $(LDSO) $@ -o $@ $^ $(EXTRALIBS)
else
	$(Q_LD)$(CXX) $(LDSO) -o $@ $^ $(EXTRALIBS)

endif	

install:
	mkdir -p $(DESTDIR)$(prefix)/$(AC_PLUGIN_DIR)
	install $(PLUGIN) $(DESTDIR)$(prefix)/$(AC_PLUGIN_DIR)

uninstall:
	rm -f $(DESTDIR)$(prefix)/$(AC_PLUGIN_DIR)/$(PLUGIN)

clean:
	rm -f $(OBJECTS) $(PLUGIN)

###########################################
