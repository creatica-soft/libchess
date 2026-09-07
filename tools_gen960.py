#!/usr/bin/env python3
"""Generate all 960 Chess960 starting positions, in both FEN castling conventions.

Scharnagl numbering (SP-0..SP-959), the same numbering lichess and Stockfish use.
SP-518 must come out as the standard array RNBQKBNR; that is the check that the
decomposition is right.
"""

KNIGHT_TABLE = [(0, 1), (0, 2), (0, 3), (0, 4),
                (1, 2), (1, 3), (1, 4),
                (2, 3), (2, 4),
                (3, 4)]

def sp_to_backrank(sp: int) -> str:
    assert 0 <= sp < 960
    n = sp
    row = [None] * 8

    # Light-square bishop: files b, d, f, h (odd indices)
    n, r = divmod(n, 4)
    row[2 * r + 1] = 'B'
    # Dark-square bishop: files a, c, e, g (even indices)
    n, r = divmod(n, 4)
    row[2 * r] = 'B'
    # Queen goes on the r-th still-empty square
    n, r = divmod(n, 6)
    empties = [i for i, c in enumerate(row) if c is None]
    row[empties[r]] = 'Q'
    # The remaining 5 squares: knights by table, then R K R in order
    empties = [i for i, c in enumerate(row) if c is None]
    n1, n2 = KNIGHT_TABLE[n]
    row[empties[n1]] = 'N'
    row[empties[n2]] = 'N'
    rest = [i for i, c in enumerate(row) if c is None]
    assert len(rest) == 3
    row[rest[0]] = 'R'
    row[rest[1]] = 'K'
    row[rest[2]] = 'R'
    return ''.join(row)


def fens(sp: int):
    """Return (shredder_fen, xfen) for starting position sp."""
    back = sp_to_backrank(sp)
    rooks = [i for i, c in enumerate(back) if c == 'R']
    king = back.index('K')
    ks = max(r for r in rooks if r > king)   # kingside rook file index
    qs = min(r for r in rooks if r < king)   # queenside rook file index
    files = 'abcdefgh'
    board = f"{back.lower()}/pppppppp/8/8/8/8/PPPPPPPP/{back}"
    # Shredder-FEN: the rook FILES, kingside first, white uppercase.
    shredder = f"{files[ks].upper()}{files[qs].upper()}{files[ks]}{files[qs]}"
    # X-FEN: plain KQkq, meaning "the outermost rook on each side".
    return (f"{board} w {shredder} - 0 1", f"{board} w KQkq - 0 1")


if __name__ == "__main__":
    import sys
    assert sp_to_backrank(518) == "RNBQKBNR", sp_to_backrank(518)
    assert sp_to_backrank(0) == "BBQNNRKR", sp_to_backrank(0)
    assert sp_to_backrank(959) == "RKRNNQBB", sp_to_backrank(959)
    # Every position must be legal Chess960: bishops on opposite colours, king between rooks.
    for sp in range(960):
        b = sp_to_backrank(sp)
        bish = [i for i, c in enumerate(b) if c == 'B']
        assert bish[0] % 2 != bish[1] % 2, (sp, b)
        r = [i for i, c in enumerate(b) if c == 'R']
        assert r[0] < b.index('K') < r[1], (sp, b)
        assert sorted(b) == sorted("RNBQKBNR"), (sp, b)
    which = sys.argv[1] if len(sys.argv) > 1 else "shredder"
    for sp in range(960):
        s, x = fens(sp)
        print(s if which == "shredder" else x)
