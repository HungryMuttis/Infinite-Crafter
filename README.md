# Automating [Infinite Craft](https://neal.fun/infinite-craft/)
This is a program to automate Infinite Craft.

## Info:
On the first run, 2 files are created:
1. elements.bin (stores all discovered elements)
2. recipes.bin (stores all of tried recipes)

## Files YOU should create:
1. headers.txt (cloudflare often blocks requests without headers)
2. proxies.txt (its very easy to reach the rate limit, you should use proxies)

## Decoding storage files
In the future, I may make a decoder to see how to craft specific items, but currently, you can look inside `src/Storage.hpp` to see the file structure.
