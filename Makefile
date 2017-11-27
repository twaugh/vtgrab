PROGRAM=vtgrab
CFLAGS=-ggdb

$(PROGRAM): $(PROGRAM).c
	gcc $(CFLAGS) -Wall $< -o $@

clean:
	-$(RM) $(PROGRAM)

distclean: clean
	-$(RM) *~

.PHONY: clean distclean update
