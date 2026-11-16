#[inline(always)]
fn leaf(value: i32) -> i32 {
    let address = 0x10 as *mut i32;
    unsafe {
        std::ptr::write_volatile(address, value);
    }
    value + 1
}

#[inline(never)]
fn crash_here(value: i32) -> i32 {
    leaf(value)
}

fn main() {
    std::hint::black_box(crash_here(7));
}
