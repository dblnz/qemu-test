use proc_macro::TokenStream;
use quote::quote;
use syn::{ItemFn, parse_macro_input};

/// Wraps a test function to print `TEST: <name>` before running
/// and `PASS: <name>` or `FAIL: <name>` after.
#[proc_macro_attribute]
pub fn test_fn(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as ItemFn);
    let name = &input.sig.ident;
    let name_str = name.to_string();
    let block = &input.block;
    let vis = &input.vis;
    let sig = &input.sig;
    let attrs = &input.attrs;

    let expanded = quote! {
        #(#attrs)*
        #vis #sig {
            println!("TEST: {}", #name_str);
            let result = (|| #block)();
            match &result {
                Ok(()) => println!("PASS: {}", #name_str),
                Err(e) => eprintln!("FAIL: {}: {e}", #name_str),
            }
            result
        }
    };

    expanded.into()
}
