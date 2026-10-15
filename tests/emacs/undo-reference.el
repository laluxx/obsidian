;;; undo-reference.el --- GNU Emacs undo behavior checks for Obsidian -*- lexical-binding: t; -*-
;; Optional executable reference, independent of any Obsidian C implementation.
;; emacs -Q --batch -l tests/emacs/undo-reference.el -f ert-run-tests-batch-and-exit
(require 'ert)

(ert-deftest obs-emacs-undo-chain-and-undo-of-undo ()
  (with-temp-buffer
    (buffer-enable-undo)
    (insert "A") (undo-boundary)
    (insert "B") (undo-boundary)
    ;; Explicit last-command emulates the interactive command loop in batch.
    (let ((last-command 'different-command)) (undo))
    (should (equal (buffer-string) "A"))
    (let ((last-command 'undo)) (undo))
    (should (equal (buffer-string) ""))
    ;; Any intervening non-undo command breaks the consecutive undo chain.
    (let ((last-command 'forward-char)) (undo))
    (should (equal (buffer-string) "A"))
    (let ((last-command 'undo)) (undo))
    (should (equal (buffer-string) ""))))

(ert-deftest obs-emacs-undo-only-skips-redo-records ()
  (with-temp-buffer
    (buffer-enable-undo)
    (insert "A") (undo-boundary)
    (insert "B") (undo-boundary)
    (let ((last-command 'other)) (undo))
    (should (equal (buffer-string) "A"))
    (let ((last-command 'undo)) (undo-only))
    (should (equal (buffer-string) ""))))

(ert-deftest obs-emacs-undo-redo-consumes-undo-record ()
  (with-temp-buffer
    (buffer-enable-undo)
    (insert "A") (undo-boundary)
    (let ((last-command 'other)) (undo))
    (should (equal (buffer-string) ""))
    (undo-redo)
    (should (equal (buffer-string) "A"))
    (should-error (undo-redo))))

(ert-deftest obs-emacs-explicit-change-groups ()
  (with-temp-buffer
    (buffer-enable-undo)
    (insert "A")
    (insert "B")
    (undo-boundary)
    (let ((last-command 'other)) (undo))
    (should (equal (buffer-string) ""))))
;;; undo-reference.el ends here
