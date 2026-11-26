/* Same compiled Objective-C selector ABI on both the lazy bundle and its
 * actual frontend. Used only on the pinned iOS build when system metadata
 * for this private selector is absent. A present mismatch always refuses. */
#ifndef S9_P12751_LAYOUT_ABI_H
#define S9_P12751_LAYOUT_ABI_H
@protocol S9P12751LayoutABI
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed;
@end
#endif
