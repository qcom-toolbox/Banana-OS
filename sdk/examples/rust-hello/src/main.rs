// SPDX-License-Identifier: BSD-2-Clause
//! A Rust program for Banana OS, built with sdk/tools/banana-cargo:
//!     banana-cargo package --release      ->  rust-hello.bpk
//! then on Banana OS: pkg install rust-hello.bpk ; rust-hello [names...]
use std::collections::BTreeMap;
use std::sync::{mpsc, Arc, Mutex};
use std::time::Instant;
use std::{env, fs, thread};

fn main() {
    let start = Instant::now();
    let names: Vec<String> = env::args().skip(1).collect();
    let who = if names.is_empty() { "Banana OS".to_string() } else { names.join(", ") };
    println!("Hello, {who}! (Rust {})", option_env!("CARGO_PKG_RUST_VERSION").filter(|v| !v.is_empty()).unwrap_or("std"));

    // files and folders
    let dir = env::temp_dir().join("rust-hello");
    fs::create_dir_all(&dir).expect("create a folder");
    for (i, word) in ["banana", "kiwi", "mango"].iter().enumerate() {
        fs::write(dir.join(format!("{i}-{word}.txt")), word.repeat(i + 1)).expect("write a file");
    }
    let mut sizes = BTreeMap::new();
    for entry in fs::read_dir(&dir).expect("read the folder") {
        let entry = entry.expect("an entry");
        sizes.insert(entry.file_name().to_string_lossy().into_owned(), entry.metadata().expect("its size").len());
    }
    println!("files in {}: {:?}", dir.display(), sizes);
    fs::remove_dir_all(&dir).expect("remove the folder");

    // threads: four of them sum a range, a channel collects
    let total = Arc::new(Mutex::new(0u64));
    let (tx, rx) = mpsc::channel();
    let workers: Vec<_> = (0..4u64)
        .map(|n| {
            let (total, tx) = (Arc::clone(&total), tx.clone());
            thread::spawn(move || {
                let part: u64 = (n * 250_000..(n + 1) * 250_000).sum();
                *total.lock().unwrap() += part;
                tx.send(n).unwrap();
            })
        })
        .collect();
    drop(tx);
    for w in workers {
        w.join().unwrap();
    }
    let done: Vec<u64> = rx.iter().collect();
    println!("4 threads summed 0..1000000 = {} ({} reported)", total.lock().unwrap(), done.len());
    println!("took {} ms", start.elapsed().as_millis());
}
