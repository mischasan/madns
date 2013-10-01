include rules.mk
export madns   ?= .
#---------------- PUBLIC VARS (used by "make install")
madns.bin	= $(madns)/hostip
madns.include   = $(madns)/madns.h
madns.lib       = $(madns)/madns.o
madns.test	= $(madns)/madns_t

#---------------- PUBLIC TARGETS (see rules.mk):
all     .PHONY  : madns.all
test	.PHONY	: madns.test
install         : madns.install

#---------------- PRIVATE RULES:
madns.all      	: $(madns.bin) 
madns.install	: madns.all
madns.test	: $(madns.test:%=%.pass)

$(madns)/hostip	: $(madns)/madns.o

$(madns.test)	: LDLIBS += -pthread
$(madns.test)   : $(madns)/madns.o $(madns)/tap.o

.INTERMEDIATE	: $(madns)/madns.o $(madns)/madns_t.o $(madns)/hostip.o

-include $(madns)/*.d
