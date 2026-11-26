.section .rodata
.balign 8
.global _binary_classes_dex_start
.hidden _binary_classes_dex_start
_binary_classes_dex_start:
.incbin "classes.dex"
.global _binary_classes_dex_end
.hidden _binary_classes_dex_end
_binary_classes_dex_end:
.section .note.GNU-stack,"",@progbits
