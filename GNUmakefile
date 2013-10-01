include rules.mk
export madns   ?= .
#---------------- PUBLIC VARS (used by "make install")
madns.bin	= $(madns)/hostip
madns.include   = $(madns)/madns.h
madns.lib       = $(madns)/madns.o

#---------------- PUBLIC TARGETS (see rules.mk):
all     .PHONY  : madns.all
test	.PHONY	: madns.test
install         : madns.install

#---------------- PRIVATE RULES:
madns.all      : $(madns.bin) 
madns.install	: madns.all
madns.test	:;@echo No tests defined for madns; false

$(madns)/hostip: $(madns)/madns.o

-include $(madns)/*.d
