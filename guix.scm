;;; ethreads/guix.scm --- Guix package for ethreads
;;;
;;; C++23 task scheduler + coroutine runtime built on io_uring.  Builds the
;;; static archive libethreads.a and installs the public headers.
;;;
;;; Cross-repo siblings are referenced via (load "../<dep>/guix.scm"); the flat
;;; frameworks/<repo> layout is a hard invariant (see the migration design doc).
;;; Third-party deps come from Guix (liburing, mimalloc) via guix/lib.scm.

(use-modules (guix packages)
             (guix gexp)
             (guix git)
             (guix git-download)
             (guix build-system gnu)
             (guix build-system copy)
             ((guix licenses) #:prefix license:))

(define %here (dirname (current-filename)))

;; Shared helpers + custom/static packages (repo-source, mimalloc, liburing,
;; gcc-toolchain, %frameworks-native-inputs, ...).
(load (string-append %here "/../guix/lib.scm"))

;; Sibling e* dependency: header-only exstd (copy-build-system, installs headers
;; under include/).  Referenced relative to this file's location.
(define exstd (load (string-append %here "/../exstd/guix.scm")))

;; C++23 coroutines/io_uring: gcc-toolchain (gcc 15.2) handles them and ships
;; gcc-ar/gcc-ranlib.  Comes from %frameworks-native-inputs (guix/lib.scm).

(package
  (name "ethreads")
  (version "0.1.0")
  (source (repo-source %here "ethreads-src"))
  (build-system gnu-build-system)
  (arguments
   (list
    #:tests? #f
    #:make-flags
    #~(list (string-append "PREFIX=" #$output)
            "CXX=g++"
            "AR=gcc-ar")
    #:phases
    #~(modify-phases %standard-phases
        (delete 'configure)
        ;; Build only the static library by default; the test/benchmark
        ;; binaries are dev-only and pull in extra runtime deps.
        (replace 'build
          (lambda* (#:key make-flags #:allow-other-keys)
            (apply invoke "make" "lib" make-flags))))))
  ;; Static-linked deps: exstd (sibling), mimalloc, liburing.  glibc stays
  ;; dynamic.  Headers/archives are exposed on CPATH / LIBRARY_PATH.
  (inputs (cons exstd %ethreads-3p))   ; exstd + mimalloc-static + liburing-static + zlib
  (native-inputs %frameworks-native-inputs)
  (synopsis "C++23 task scheduler and coroutine runtime over io_uring")
  (description
   "ethreads is a C++23 threading library providing a work-stealing task
scheduler, coroutine-based tasks, an async runtime, timer and io_uring I/O
services, and shared-state primitives.  It links statically against mimalloc
and liburing (mostly-static link policy; glibc stays dynamic).")
  (home-page "https://github.com/RealAstolfo/ethreads")
  (license license:expat))
