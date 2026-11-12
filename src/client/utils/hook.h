/* Code patching helpers. */
#ifndef R1_HOOK_H
#define R1_HOOK_H

/* Replace the target of an E8 call at a fixed site. The site must currently call 'expect'.
 * Both addresses are IDA addresses; *orig (if given) receives the original target. */
int patch_call(unsigned site_va, unsigned expect_va, void *hook, void **orig);

#endif
