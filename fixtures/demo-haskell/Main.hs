module Main where

import Foreign.Ptr (intPtrToPtr)
import Foreign.Storable (poke)
import System.IO (hPutStrLn, stderr)

{-# NOINLINE leaf #-}
leaf :: Int -> IO ()
leaf value = poke (intPtrToPtr 0x10) value

{-# NOINLINE outer #-}
outer :: Int -> IO ()
outer value = leaf value

main :: IO ()
main = do
  hPutStrLn stderr "haskell fixture"
  outer 7
