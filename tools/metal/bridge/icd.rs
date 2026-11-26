// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the HakiOS authors (Scion). See docs/metal-plan.md, "Metal on Vulkan".
//! The Vulkan driver under the runtime: an installable client driver (ICD) opened by its path, with no Khronos loader between.
//!
//! The runtime opens "a Vulkan loader" by path and takes the library's `vkGetInstanceProcAddr` (ash's `Entry::load_from`). iOS has
//! no loader, and an ICD need not export that name: the loader-ICD interface's own entry is `vk_icdGetInstanceProcAddr`, which is
//! all Mesa's drivers export (lavapipe on a Mac, panvk) and what gfxstream's guest driver for Scion (libhakios.vk.dylib) is built
//! with. So this library exports `vkGetInstanceProcAddr` itself, forwarding to the driver `select` opened, and the runtime is given
//! this library's own path as its "loader": opening it again gives the same image, whose `vkGetInstanceProcAddr` is the one below.
//!
//! Where the driver has no coherent host-visible memory, the runtime flushes and invalidates itself (Scion's patch to it,
//! tools/metal/m2v/); coherence.rs can stand in for coherent memory between the two instead (`HAKIOS_VK_COHERENCE=emulate`).
//!
//! What a loader would do and this does not: no layers, no dispatch table patched into the driver's dispatchable objects (every call
//! goes straight to the driver's entry points, which is how a driver is used without a loader), one driver per process.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::PathBuf;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::Mutex;

pub type VoidFunction = Option<unsafe extern "system" fn()>;
type GetInstanceProcAddr = unsafe extern "system" fn(*mut c_void, *const c_char) -> VoidFunction;
type Negotiate = unsafe extern "system" fn(*mut u32) -> i32;

/// The loader-ICD interface version asked for: 5 is what Mesa's drivers and the Khronos loader of 2020 on agree on. The answer only
/// tells this file what the driver expects of a loader, and nothing here depends on more than version 1 (`vk_icdGetInstanceProcAddr`).
const INTERFACE_VERSION: u32 = 5;

/// The driver's entry, once a driver is selected; null before.
static ENTRY: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());

struct Selected {
    path: CString,
    entry_name: &'static str,
    interface: u32,
}
static SELECTED: Mutex<Option<Selected>> = Mutex::new(None);

unsafe extern "C" {
    fn dlopen(path: *const c_char, mode: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, name: *const c_char) -> *mut c_void;
    fn dlerror() -> *const c_char;
}
const RTLD_NOW: c_int = 0x2;
const RTLD_LAZY: c_int = 0x1;
const RTLD_LOCAL: c_int = 0x4;
pub type Observer = Option<extern "C" fn(u32,u32,u32)>;
thread_local! {
    static OBSERVER: std::cell::Cell<Observer> = const { std::cell::Cell::new(None) };
    static SEEN: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
}
pub fn with_observer<T>(observer: Observer, f: impl FnOnce()->T)->T {
    struct Reset(Observer,u32);
    impl Drop for Reset {fn drop(&mut self){OBSERVER.with(|v|v.set(self.0));SEEN.with(|v|v.set(self.1));}}
    let _reset=Reset(OBSERVER.with(|v|v.replace(observer)),SEEN.with(|v|v.replace(0)));
    f()
}
fn first(stage:u32)->Observer {
    let f=OBSERVER.with(|v|v.get());
    if f.is_none(){return None;}
    let bit=1u32<<(stage-16);
    if SEEN.with(|s|{let old=s.get();s.set(old|bit);old&bit!=0}){None}else{f}
}
fn begin(observer:Observer,stage:u32){if let Some(f)=observer{f(stage,1,0);}}
fn end(observer:Observer,stage:u32,ok:bool){if let Some(f)=observer{f(stage,0,ok as u32);}}
static ENUM_EXT: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
/* Exact Vulkan ABI, only installed for the instance-extension enumeration
 * entry returned by the selected driver. No synthetic Vulkan calls. */
unsafe extern "system" fn observe_enum_ext(layer:*const c_char,count:*mut u32,props:*mut ash::vk::ExtensionProperties)->ash::vk::Result {
    let ptr=ENUM_EXT.load(Ordering::Acquire);
    if ptr.is_null(){return ash::vk::Result::ERROR_INITIALIZATION_FAILED;}
    let f:ash::vk::PFN_vkEnumerateInstanceExtensionProperties=unsafe{std::mem::transmute(ptr)};
    let obs=first(19);begin(obs,19);
    let rc=unsafe{f(layer,count,props)};
    end(obs,19,rc.as_raw()>=0);rc
}

/// The Vulkan entry the runtime reaches (through `Entry::load_from` on this library): the selected driver's, or none.
///
/// # Safety
/// Vulkan's contract for `vkGetInstanceProcAddr`.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn vkGetInstanceProcAddr(instance: *mut c_void, name: *const c_char) -> VoidFunction {
    let entry = ENTRY.load(Ordering::Acquire);
    if entry.is_null() || name.is_null() {
        return None;
    }
    // SAFETY: ENTRY only ever holds a driver's GetInstanceProcAddr, and the driver stays loaded for the life of the process.
    let get: GetInstanceProcAddr = unsafe { std::mem::transmute(entry) };
    // a driver without coherent host memory gets it stood in for (coherence.rs); HAKIOS_VK_INCOHERENT=1 makes any driver one
    // (incoherent.rs, a test)
    let symbol = unsafe { CStr::from_ptr(name) };
    let general=first(16);
    let stage=match symbol.to_bytes(){b"vkEnumerateInstanceExtensionProperties"=>17,b"vkCreateInstance"=>18,b"vkEnumeratePhysicalDevices"=>22,_=>0};
    let specific=if stage!=0{first(stage)}else{None};
    begin(general,16);if stage!=0{begin(specific,stage);}
    let driver = unsafe { get(instance, name) };
    if stage!=0{end(specific,stage,driver.is_some());}end(general,16,driver.is_some());
    let wrapped=crate::coherence::wrap(symbol, crate::incoherent::wrap(symbol, driver));
    if stage==17 && OBSERVER.with(|v|v.get().is_some()){
        if let Some(raw)=wrapped{
            let ptr=raw as *mut c_void;
            let old=ENUM_EXT.compare_exchange(std::ptr::null_mut(),ptr,Ordering::AcqRel,Ordering::Acquire);
            if old.is_ok() || old==Err(ptr){
                let f:ash::vk::PFN_vkEnumerateInstanceExtensionProperties=observe_enum_ext;
                return Some(unsafe{std::mem::transmute(f)});
            }
        }
    }
    wrapped
}

fn last_dl_error() -> String {
    // SAFETY: dlerror's string is valid until the next dl call on this thread.
    let text = unsafe { dlerror() };
    if text.is_null() { "unknown".into() } else { unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned() }
}

/// This library's own file: what the runtime is told to open as its loader.
fn own_path() -> Result<PathBuf, String> {
    #[repr(C)]
    struct DlInfo {
        fname: *const c_char,
        fbase: *mut c_void,
        sname: *const c_char,
        saddr: *mut c_void,
    }
    unsafe extern "C" {
        fn dladdr(address: *const c_void, info: *mut DlInfo) -> c_int;
    }
    let mut info = DlInfo { fname: std::ptr::null(), fbase: std::ptr::null_mut(), sname: std::ptr::null(), saddr: std::ptr::null_mut() };
    // SAFETY: an address inside this image and a writable record.
    if unsafe { dladdr(vkGetInstanceProcAddr as *const c_void, &mut info) } == 0 || info.fname.is_null() {
        return Err("this library's own path (dladdr) is not known".into());
    }
    // SAFETY: dladdr's file name lives as long as the image.
    Ok(PathBuf::from(unsafe { CStr::from_ptr(info.fname) }.to_string_lossy().into_owned()))
}

/// Opens the driver at `path` (once per process: the same path again is the same driver, another one is refused, since devices of
/// the first may exist) and returns the path to give the runtime as its loader.
pub fn select(path: &CStr) -> Result<PathBuf, String> {select_observed(path,None,false)}
pub fn select_observed(path: &CStr,observer:Observer,lazy:bool) -> Result<PathBuf,String>{
    begin(observer,9);
    let locked=SELECTED.lock().map_err(|_| "the driver lock is poisoned".to_string());
    end(observer,9,locked.is_ok());
    let mut selected=locked?;
    if let Some(s) = selected.as_ref() {
        if s.path.as_c_str() != path {
            return Err(format!("a Vulkan driver is already in use ({}); {} is refused", s.path.to_string_lossy(), path.to_string_lossy()));
        }
        begin(observer,14);let result=own_path();end(observer,14,result.is_ok());return result;
    }
    // SAFETY: a C string; the handle is kept for the life of the process (never closed).
    begin(observer,11);
    let handle = unsafe { dlopen(path.as_ptr(), (if lazy {RTLD_LAZY}else{RTLD_NOW}) | RTLD_LOCAL) };
    end(observer,11,!handle.is_null());
    if handle.is_null() {
        return Err(format!("the Vulkan driver {} does not open: {}", path.to_string_lossy(), last_dl_error()));
    }
    let mut entry = std::ptr::null_mut();
    let mut entry_name = "";
    begin(observer,12);
    for name in [c"vk_icdGetInstanceProcAddr", c"vkGetInstanceProcAddr"] {
        // SAFETY: a loaded image and a C string.
        entry = unsafe { dlsym(handle, name.as_ptr()) };
        if !entry.is_null() {
            entry_name = if name == c"vk_icdGetInstanceProcAddr" { "vk_icdGetInstanceProcAddr" } else { "vkGetInstanceProcAddr" };
            break;
        }
    }
    end(observer,12,!entry.is_null());
    if entry.is_null() {
        return Err(format!("{} exports neither vk_icdGetInstanceProcAddr nor vkGetInstanceProcAddr", path.to_string_lossy()));
    }
    let mut interface = 0;
    // SAFETY: as above.
    begin(observer,15);
    let negotiate = unsafe { dlsym(handle, c"vk_icdNegotiateLoaderICDInterfaceVersion".as_ptr()) };
    end(observer,15,!negotiate.is_null());
    if !negotiate.is_null() {
        let mut version = INTERFACE_VERSION;
        // SAFETY: the loader-ICD interface's negotiation call, with a writable version.
        let negotiate: Negotiate = unsafe { std::mem::transmute(negotiate) };
        begin(observer,13);let rc=unsafe { negotiate(&mut version) };end(observer,13,rc==0);
        if rc != 0 {
            return Err(format!("{} refused the loader-ICD interface (version {INTERFACE_VERSION})", path.to_string_lossy()));
        }
        interface = version;
    }
    ENTRY.store(entry, Ordering::Release);
    *selected = Some(Selected { path: path.to_owned(), entry_name, interface });
    begin(observer,14);let result=own_path();end(observer,14,result.is_ok());result
}

/// One line on the selected driver: its path, the entry used and the negotiated interface version (0: not negotiated).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_icd_describe(out: *mut c_char, size: usize) -> usize {
    let text = match SELECTED.lock().ok().and_then(|s| s.as_ref().map(|s| (s.path.clone(), s.entry_name, s.interface))) {
        Some((path, entry, interface)) => {
            let mut text = format!("{} ({entry}, ICD interface {interface}{})", path.to_string_lossy(),
                if crate::coherence::emulating() { "; coherent host memory emulated" } else { "" });
            if crate::incoherent::on() {
                let [flushed, flushes, invalidated, invalidates] = crate::incoherent::moved();
                text += &format!("; test HAKIOS_VK_INCOHERENT: {flushed} bytes flushed in {flushes} calls, {invalidated} invalidated in \
                                  {invalidates}");
            }
            text
        }
        None => "no Vulkan driver selected".into(),
    };
    if !out.is_null() && size > 0 {
        let n = text.len().min(size - 1);
        // SAFETY: the caller's buffer of `size` bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), out as *mut u8, n);
            *out.add(n) = 0;
        }
    }
    text.len()
}
