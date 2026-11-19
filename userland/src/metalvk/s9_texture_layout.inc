/* S9 experimental Metal SPI: opt-in via exact boot-arg metal_layout=1
 * or HAKIOS_METAL_LAYOUT=1 in the process environment. Default behavior
 * remains the upstream nil/refusal, so this does not risk a stable boot.
 *
 * Based on the texture layout approach documented in DenialOfGravity/ios-redfin
 * phone/metal2/NativeBridgeMetalFrontend.mm (SweetTextureLayout). Only 2D,
 * one level, one sample, one slice. No guessed depth/PVRTC layout.
 */
#include "s9_texture_layout.h"
#include <sys/sysctl.h>

static bool s9LayoutOn(void) {
    static const bool on=[]{
        if(const char *e=getenv("HAKIOS_METAL_LAYOUT")) return !strcmp(e,"1");
        char args[4096]={};size_t size=sizeof(args)-1;
        if(sysctlbyname("kern.bootargs",args,&size,nullptr,0)!=0) return false;
        args[std::min(size,sizeof(args)-1)]=0;
        for(const char *p=strstr(args,"metal_layout=");p;p=strstr(p+1,"metal_layout=")) {
            const char *v=p+13;   // strlen("metal_layout=")
            if((p==args || p[-1]==' ' || p[-1]=='\t') &&
               *v=='1' && (!v[1] || v[1]==' ' || v[1]=='\t'))return true;
        }
        return false;
    }();
    return on;
}
static bool s9LayoutDescriptor(MTLTextureDescriptor *d,s9_texture_layout *out) {
    if(!d || d.textureType!=MTLTextureType2D || d.depth!=1 ||
       d.width==0 || d.height==0 || d.mipmapLevelCount!=1 ||
       d.arrayLength!=1 || d.sampleCount!=1) return false;
    uint32_t bw=0,bh=0,bytes=0;
    if(!s9_tex_block((uint32_t)d.pixelFormat,&bw,&bh,&bytes))return false;
    // Compressed textures use packed block rows; other formats MUST use the
    // device driver's actual linear pitch (not a guessed 4x width).
    const uint64_t pitch=(bw>1 || bh>1)?0:
        nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
    if(bw==1 && bh==1 && !pitch)return false;
    return s9_tex_plan((uint32_t)d.pixelFormat,d.width,d.height,
                       pitch,(uint64_t)getpagesize(),out)!=0;
}

typedef struct {uint64_t words[2];} S9TextureWatermark;
@interface S9TextureLayout : NSObject
- (instancetype)initWithPlan:(s9_texture_layout)plan;
@end
@implementation S9TextureLayout {
    s9_texture_layout _plan;
}
- (instancetype)initWithPlan:(s9_texture_layout)p {
    if((self=[super init]))_plan=p;
    return self;
}
- (NSUInteger)size {return (NSUInteger)_plan.size;}
- (NSUInteger)alignment {return (NSUInteger)_plan.alignment;}
- (MTLSize)copyGranularity {return MTLSizeMake(_plan.block_width,_plan.block_height,1);}
- (S9TextureWatermark)watermark {S9TextureWatermark w={{0,0}};return w;}
- (void)initializeTextureMemory:(void *)memory {(void)memory;}
- (void)finalizeTextureMemory:(void *)memory {(void)memory;}
- (void)copyFromLinearBytes:(const void *)source linearOffset:(NSUInteger)offset
         linearBytesPerRow:(NSUInteger)sourceRow linearBytesPerImage:(NSUInteger)image
         toTextureMemory:(void *)dest textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level
         textureRegion:(MTLRegion)region {
    (void)image;
    uint64_t start=0,n=0,rows=0;
    if(!source || !dest || slice || level ||
       !s9_tex_region(&_plan,region.origin.x,region.origin.y,region.size.width,region.size.height,
                      &start,&n,&rows) || sourceRow<n ||
       (rows && sourceRow>SIZE_MAX/(rows-1?rows-1:1)) ||
       offset>SIZE_MAX-(rows-1)*sourceRow ||
       n>SIZE_MAX-offset-(rows-1)*sourceRow)return;
    for(uint64_t i=0;i<rows;++i)
        memcpy((uint8_t *)dest+start+i*_plan.row_bytes,
               (const uint8_t *)source+offset+i*sourceRow,(size_t)n);
}
- (void)copyFromTextureMemory:(const void *)source textureSlice:(NSUInteger)slice
                textureLevel:(NSUInteger)level textureRegion:(MTLRegion)region
                toLinearBytes:(void *)dest linearOffset:(NSUInteger)offset
                linearBytesPerRow:(NSUInteger)destRow linearBytesPerImage:(NSUInteger)image {
    (void)image;
    uint64_t start=0,n=0,rows=0;
    if(!source || !dest || slice || level ||
       !s9_tex_region(&_plan,region.origin.x,region.origin.y,region.size.width,region.size.height,
                      &start,&n,&rows) || destRow<n ||
       (rows && destRow>SIZE_MAX/(rows-1?rows-1:1)) ||
       offset>SIZE_MAX-(rows-1)*destRow ||
       n>SIZE_MAX-offset-(rows-1)*destRow)return;
    for(uint64_t i=0;i<rows;++i)
        memcpy((uint8_t *)dest+offset+i*destRow,
               (const uint8_t *)source+start+i*_plan.row_bytes,(size_t)n);
}
@end

@interface S9TextureMemoryOwner : NSObject
@property(nonatomic,copy) void (^deallocator)(void *,NSUInteger);
@property(nonatomic) void *pointer;
@property(nonatomic) NSUInteger length;
@end
@implementation S9TextureMemoryOwner
- (void)dealloc {if(_deallocator)_deallocator(_pointer,_length);}
@end
static char s9TextureMemoryOwnerKey;
static std::atomic<unsigned> s9LayoutRequests{0},s9LayoutAccepts{0},s9NoCopyAccepts{0};
