LAB = malloclab
LAB_CHECKPOINT = malloclabcheckpoint
LAB_FINAL = malloclab
COURSECODE = 15213-m20 15513-m20
SAN_LIBRARY_PATH = /afs/cs.cmu.edu/academic/class/15213/lib/
ifneq (,$(wildcard autograde-lib/))
  SAN_LIBRARY_PATH = autograde-lib/
endif
