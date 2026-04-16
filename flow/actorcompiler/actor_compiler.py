#!/usr/bin/env python3
"""
Python implementation of the Flow actor compiler.

This mirrors the behavior of the original C# actor compiler that transforms
`*.actor.cpp` and `*.actor.h` sources into their generated `*.actor.g.*`
counterparts. The goal is to produce byte-for-byte identical output to the
existing implementation so the generated files match those in `pregenerated/`.
"""

from __future__ import annotations

import argparse
import io
import os
import re
import sys
from dataclasses import dataclass, field
from itertools import chain
from typing import Callable, Iterable, Iterator, List, Optional, Sequence, Tuple


class Error(Exception):
    def __init__(self, source_line: int, message: str, *args: object) -> None:
        super().__init__(message.format(*args))
        self.SourceLine = source_line


class ErrorMessagePolicy:
    def __init__(self) -> None:
        self.DisableDiagnostics = False

    def HandleActorWithoutWait(self, source_file: str, actor: "Actor") -> None:
        if not self.DisableDiagnostics and not actor.isTestCase:
            # TODO(atn34): Once cmake is the only build system we can make this an error instead of a warning.
            sys.stderr.write(
                f"{source_file}:{actor.SourceLine}: warning: ACTOR {actor.name} does not contain a wait() statement\n"
            )

    def ActorsNoDiscardByDefault(self) -> bool:
        return not self.DisableDiagnostics


@dataclass
class VarDeclaration:
    type: str = ""
    name: str = ""
    initializer: str = ""
    initializerConstructorSyntax: bool = False


class Statement:
    FirstSourceLine: int

    def containsWait(self) -> bool:
        return False


@dataclass
class PlainOldCodeStatement(Statement):
    code: str


@dataclass
class StateDeclarationStatement(Statement):
    decl: VarDeclaration

    def __str__(self) -> str:
        if self.decl.initializerConstructorSyntax:
            return f"State {self.decl.type} {self.decl.name}({self.decl.initializer});"
        return f"State {self.decl.type} {self.decl.name} = {self.decl.initializer};"


@dataclass
class WhileStatement(Statement):
    expression: str
    body: Statement

    def containsWait(self) -> bool:
        return self.body.containsWait()


@dataclass
class ForStatement(Statement):
    initExpression: str = ""
    condExpression: str = ""
    nextExpression: str = ""
    body: Statement = None  # type: ignore[assignment]

    def containsWait(self) -> bool:
        return self.body.containsWait()


@dataclass
class RangeForStatement(Statement):
    rangeExpression: str
    rangeDecl: str
    body: Statement

    def containsWait(self) -> bool:
        return self.body.containsWait()


@dataclass
class LoopStatement(Statement):
    body: Statement

    def __str__(self) -> str:  # pragma: no cover - debug helper
        return "Loop " + str(self.body)

    def containsWait(self) -> bool:
        return self.body.containsWait()


class BreakStatement(Statement):
    pass


class ContinueStatement(Statement):
    pass


@dataclass
class IfStatement(Statement):
    expression: str
    constexpr: bool
    ifBody: Statement
    elseBody: Optional[Statement] = None

    def containsWait(self) -> bool:
        return self.ifBody.containsWait() or (
            self.elseBody is not None and self.elseBody.containsWait()
        )


@dataclass
class ReturnStatement(Statement):
    expression: str

    def __str__(self) -> str:  # pragma: no cover - debug helper
        return "Return " + self.expression


@dataclass
class WaitStatement(Statement):
    result: VarDeclaration
    futureExpression: str
    resultIsState: bool
    isWaitNext: bool
    FirstSourceLine: int = 0

    def __str__(self) -> str:  # pragma: no cover - debug helper
        return f"Wait {self.result.type} {self.result.name} <- {self.futureExpression} ({'state' if self.resultIsState else 'local'})"

    def containsWait(self) -> bool:
        return True


@dataclass
class ChooseStatement(Statement):
    body: Statement

    def __str__(self) -> str:  # pragma: no cover - debug helper
        return "Choose " + str(self.body)

    def containsWait(self) -> bool:
        return self.body.containsWait()


@dataclass
class WhenStatement(Statement):
    wait: WaitStatement
    body: Statement

    def __str__(self) -> str:  # pragma: no cover - debug helper
        return f"When ({self.wait}) {self.body}"

    def containsWait(self) -> bool:
        return True


@dataclass
class TryStatement(Statement):
    @dataclass
    class Catch:
        expression: str
        body: Statement
        FirstSourceLine: int

    tryBody: Statement
    catches: List["TryStatement.Catch"]

    def containsWait(self) -> bool:
        if self.tryBody.containsWait():
            return True
        for c in self.catches:
            if c.body.containsWait():
                return True
        return False


@dataclass
class ThrowStatement(Statement):
    expression: str


@dataclass
class CodeBlock(Statement):
    statements: Sequence[Statement]

    def containsWait(self) -> bool:
        return any(stmt.containsWait() for stmt in self.statements)


@dataclass
class Declaration:
    type: str
    name: str
    comment: str


@dataclass
class Actor:
    attributes: List[str] = field(default_factory=list)
    returnType: Optional[str] = None
    name: str = ""
    enclosingClass: Optional[str] = None
    parameters: Sequence[VarDeclaration] = field(default_factory=list)
    templateFormals: Optional[Sequence[VarDeclaration]] = None
    body: Optional[CodeBlock] = None
    SourceLine: int = 0
    isStatic: bool = False
    testCaseParameters: Optional[str] = None
    nameSpace: Optional[str] = None
    isForwardDeclaration: bool = False
    isTestCase: bool = False
    _isUncancellable: bool = False

    def IsCancellable(self) -> bool:
        return self.returnType is not None and not self._isUncancellable

    def SetUncancellable(self) -> None:
        self._isUncancellable = True


@dataclass
class Descr:
    name: str = ""
    superClassList: Optional[str] = None
    body: List[Declaration] = field(default_factory=list)


@dataclass
class ClassContext:
    name: str
    inBlocks: int


@dataclass
class Token:
    Value: str
    Position: int = 0
    SourceLine: int = 0
    BraceDepth: int = 0
    ParenDepth: int = 0

    @property
    def IsWhitespace(self) -> bool:
        return (
            self.Value in {" ", "\n", "\r", "\r\n", "\t"}
            or self.Value.startswith("//")
            or self.Value.startswith("/*")
        )

    def Assert(self, error: str, pred: Callable[["Token"], bool]) -> "Token":
        if not pred(self):
            raise Error(self.SourceLine, error)
        return self

    def GetMatchingRangeIn(self, token_range: "TokenRange") -> "TokenRange":
        value = self.Value
        brace_depth = self.BraceDepth
        paren_depth = self.ParenDepth
        tokens = token_range.GetAllTokens()
        if value == "(":
            pred = lambda t: t.Value != ")" or t.ParenDepth != paren_depth
            direction = 1
        elif value == ")":
            pred = lambda t: t.Value != "(" or t.ParenDepth != paren_depth
            direction = -1
        elif value == "{":
            pred = lambda t: t.Value != "}" or t.BraceDepth != brace_depth
            direction = 1
        elif value == "}":
            pred = lambda t: t.Value != "{" or t.BraceDepth != brace_depth
            direction = -1
        elif value == "<":
            iterator = AngleBracketParser.NotInsideAngleBrackets(
                TokenRange(tokens, self.Position, token_range.End)
            )
            next(iterator)  # skip "<"
            match = next(iterator)
            return TokenRange(tokens, self.Position + 1, match.Position)
        elif value == "[":
            iterator = BracketParser.NotInsideBrackets(
                TokenRange(tokens, self.Position, token_range.End)
            )
            next(iterator)  # skip "["
            match = next(iterator)
            return TokenRange(tokens, self.Position + 1, match.Position)
        else:
            raise NotImplementedError("Can't match this token!")

        if direction == -1:
            rng = TokenRange(tokens, token_range.Begin, self.Position).RevTakeWhile(
                pred
            )
            if rng.Begin == token_range.Begin:
                raise Error(self.SourceLine, f"Syntax error: Unmatched {value}")
        else:
            rng = TokenRange(tokens, self.Position + 1, token_range.End).TakeWhile(pred)
            if rng.End == token_range.End:
                raise Error(self.SourceLine, f"Syntax error: Unmatched {value}")
        return rng


class TokenRange(Iterable[Token]):
    def __init__(self, tokens: Sequence[Token], begin_pos: int, end_pos: int) -> None:
        if begin_pos > end_pos:
            raise ValueError("Invalid TokenRange")
        self.tokens = tokens
        self.beginPos = begin_pos
        self.endPos = end_pos

    def __iter__(self) -> Iterator[Token]:
        for i in range(self.beginPos, self.endPos):
            yield self.tokens[i]

    @property
    def IsEmpty(self) -> bool:
        return self.beginPos == self.endPos

    @property
    def Begin(self) -> int:
        return self.beginPos

    @property
    def End(self) -> int:
        return self.endPos

    def First(self, pred: Callable[[Token], bool] = lambda t: True) -> Token:
        for i in range(self.beginPos, self.endPos):
            tok = self.tokens[i]
            if pred(tok):
                return tok
        raise Exception("Empty TokenRange")

    def Last(self, pred: Callable[[Token], bool] = lambda t: True) -> Token:
        for i in range(self.endPos - 1, self.beginPos - 1, -1):
            tok = self.tokens[i]
            if pred(tok):
                return tok
        raise Exception("Matching token not found")

    def Skip(self, count: int) -> "TokenRange":
        return TokenRange(self.tokens, self.beginPos + count, self.endPos)

    def Consume(
        self, value_or_error, pred: Optional[Callable[[Token], bool]] = None
    ) -> "TokenRange":
        if pred is None:
            value = value_or_error
            self.First().Assert(f"Expected {value}", lambda t: t.Value == value)
        else:
            error = value_or_error
            self.First().Assert(error, pred)
        return self.Skip(1)

    def SkipWhile(self, pred: Callable[[Token], bool]) -> "TokenRange":
        for e in range(self.beginPos, self.endPos):
            if not pred(self.tokens[e]):
                return TokenRange(self.tokens, e, self.endPos)
        return TokenRange(self.tokens, self.endPos, self.endPos)

    def TakeWhile(self, pred: Callable[[Token], bool]) -> "TokenRange":
        for e in range(self.beginPos, self.endPos):
            if not pred(self.tokens[e]):
                return TokenRange(self.tokens, self.beginPos, e)
        return TokenRange(self.tokens, self.beginPos, self.endPos)

    def RevTakeWhile(self, pred: Callable[[Token], bool]) -> "TokenRange":
        for e in range(self.endPos - 1, self.beginPos - 1, -1):
            if not pred(self.tokens[e]):
                return TokenRange(self.tokens, e + 1, self.endPos)
        return TokenRange(self.tokens, self.beginPos, self.endPos)

    def RevSkipWhile(self, pred: Callable[[Token], bool]) -> "TokenRange":
        for e in range(self.endPos - 1, self.beginPos - 1, -1):
            if not pred(self.tokens[e]):
                return TokenRange(self.tokens, self.beginPos, e + 1)
        return TokenRange(self.tokens, self.beginPos, self.beginPos)

    def GetAllTokens(self) -> Sequence[Token]:
        return self.tokens

    @property
    def Length(self) -> int:
        return self.endPos - self.beginPos


class BracketParser:
    @staticmethod
    def NotInsideBrackets(tokens: Iterable[Token]) -> Iterator[Token]:
        bracket_depth = 0
        base_pd: Optional[int] = None
        for tok in tokens:
            if base_pd is None:
                base_pd = tok.ParenDepth
            if tok.ParenDepth == base_pd and tok.Value == "]":
                bracket_depth -= 1
            if bracket_depth == 0:
                yield tok
            if tok.ParenDepth == base_pd and tok.Value == "[":
                bracket_depth += 1


class AngleBracketParser:
    @staticmethod
    def NotInsideAngleBrackets(tokens: Iterable[Token]) -> Iterator[Token]:
        angle_depth = 0
        base_pd: Optional[int] = None
        for tok in tokens:
            if base_pd is None:
                base_pd = tok.ParenDepth
            if tok.ParenDepth == base_pd and tok.Value == ">":
                angle_depth -= 1
            if angle_depth == 0:
                yield tok
            if tok.ParenDepth == base_pd and tok.Value == "<":
                angle_depth += 1


class ActorParser:
    LineNumbersEnabled = True

    def __init__(
        self,
        text: str,
        source_file: str,
        error_message_policy: ErrorMessagePolicy,
        generate_probes: bool,
    ):
        self.sourceFile = source_file
        self.errorMessagePolicy = error_message_policy
        self.generateProbes = generate_probes
        self.identifierPattern = re.compile(r"[a-zA-Z_][a-zA-Z_0-9]*", re.S)
        self.tokenExpressions = [
            re.compile(r"\{", re.S),
            re.compile(r"\}", re.S),
            re.compile(r"\(", re.S),
            re.compile(r"\)", re.S),
            re.compile(r"\[", re.S),
            re.compile(r"\]", re.S),
            re.compile(r"//[^\n]*", re.S),
            re.compile(r"/[*]([*][^/]|[^*])*[*]/", re.S),
            re.compile(r"'(\\.|[^'\n])*'", re.S),
            re.compile(r'"(\\.|[^"\n])*"', re.S),
            re.compile(r"[a-zA-Z_][a-zA-Z_0-9]*", re.S),
            re.compile(r"\r\n", re.S),
            re.compile(r"\n", re.S),
            re.compile(r"::", re.S),
            re.compile(r":", re.S),
            re.compile(r"#[a-z]*", re.S),
            re.compile(r".", re.S),
        ]
        self.tokens: List[Token] = [Token(Value=t) for t in self.Tokenize(text)]
        self.CountParens()

    # Utility predicates
    def Whitespace(self, tok: Token) -> bool:
        return tok.IsWhitespace

    def NonWhitespace(self, tok: Token) -> bool:
        return not tok.IsWhitespace

    def range(self, begin: int, end: int) -> TokenRange:
        return TokenRange(self.tokens, begin, end)

    def str_tokens(self, toks: Iterable[Token]) -> str:
        return "".join(t.Value for t in toks)

    # Parsing helpers
    def SplitParameterList(
        self, toks: TokenRange, delimiter: str
    ) -> Iterator[TokenRange]:
        if toks.Begin == toks.End:
            return
        while True:
            comma = next(
                (
                    t
                    for t in AngleBracketParser.NotInsideAngleBrackets(toks)
                    if t.Value == delimiter and t.ParenDepth == toks.First().ParenDepth
                ),
                None,
            )
            if comma is None:
                break
            yield self.range(toks.Begin, comma.Position)
            toks = self.range(comma.Position + 1, toks.End)
        yield toks

    def NormalizeWhitespace(self, tokens: Iterable[Token]) -> Iterator[Token]:
        in_whitespace = False
        leading = True
        for tok in tokens:
            if not tok.IsWhitespace:
                if in_whitespace and not leading:
                    yield Token(Value=" ")
                in_whitespace = False
                yield tok
                leading = False
            else:
                in_whitespace = True

    def ParseDeclaration(
        self, tokens: TokenRange
    ) -> Tuple[Token, TokenRange, Optional[TokenRange], bool]:
        initializer: Optional[TokenRange] = None
        before_initializer = tokens
        constructor_syntax = False

        equals = next(
            (
                t
                for t in AngleBracketParser.NotInsideAngleBrackets(tokens)
                if t.Value == "=" and t.ParenDepth == tokens.First().ParenDepth
            ),
            None,
        )
        if equals is not None:
            before_initializer = self.range(tokens.Begin, equals.Position)
            initializer = self.range(equals.Position + 1, tokens.End)
        else:
            paren = next(
                (
                    t
                    for t in AngleBracketParser.NotInsideAngleBrackets(tokens)
                    if t.Value == "("
                ),
                None,
            )
            if paren is not None:
                constructor_syntax = True
                before_initializer = self.range(tokens.Begin, paren.Position)
                initializer = self.range(paren.Position + 1, tokens.End).TakeWhile(
                    lambda t: t.ParenDepth > paren.ParenDepth
                )
            else:
                brace = next(
                    (
                        t
                        for t in AngleBracketParser.NotInsideAngleBrackets(tokens)
                        if t.Value == "{"
                    ),
                    None,
                )
                if brace is not None:
                    raise Error(
                        brace.SourceLine,
                        "Uniform initialization syntax is not currently supported for state variables (use '(' instead of '}' ?)",
                    )
        name = before_initializer.Last(self.NonWhitespace)
        if before_initializer.Begin == name.Position:
            raise Error(
                before_initializer.First().SourceLine, "Declaration has no type."
            )
        type_range = self.range(before_initializer.Begin, name.Position)
        return name, type_range, initializer, constructor_syntax

    def ParseVarDeclaration(self, tokens: TokenRange) -> VarDeclaration:
        name, typ, initializer, constructor = self.ParseDeclaration(tokens)
        return VarDeclaration(
            name=name.Value,
            type=self.str_tokens(self.NormalizeWhitespace(typ)),
            initializer=""
            if initializer is None
            else self.str_tokens(self.NormalizeWhitespace(initializer)),
            initializerConstructorSyntax=constructor,
        )

    def ParseDescrHeading(self, descr: Descr, toks: TokenRange) -> None:
        toks.First(self.NonWhitespace).Assert(
            "non-struct DESCR!", lambda t: t.Value == "struct"
        )
        toks = toks.SkipWhile(self.Whitespace).Skip(1).SkipWhile(self.Whitespace)

        colon = next((t for t in toks if t.Value == ":"), None)
        if colon is not None:
            super_classes = self.str_tokens(
                self.range(colon.Position + 1, toks.End)
            ).strip()
            descr.superClassList = super_classes if super_classes else None
            toks = self.range(toks.Begin, colon.Position)
        descr.name = self.str_tokens(toks).strip()

    def ParseTestCaseHeading(self, actor: Actor, toks: TokenRange) -> None:
        actor.isStatic = True
        param_range = (
            toks.Last(self.NonWhitespace)
            .Assert(
                "Unexpected tokens after test case parameter list.",
                lambda t: t.Value == ")" and t.ParenDepth == toks.First().ParenDepth,
            )
            .GetMatchingRangeIn(toks)
        )
        actor.testCaseParameters = self.str_tokens(param_range)
        actor.name = f"flowTestCase{toks.First().SourceLine}"
        actor.parameters = [
            VarDeclaration(
                name="params",
                type="UnitTestParameters",
                initializer="",
                initializerConstructorSyntax=False,
            )
        ]
        actor.returnType = "Void"

    def ParseActorHeading(self, actor: Actor, toks: TokenRange) -> None:
        template = toks.First(self.NonWhitespace)
        if template.Value == "template":
            template_params = (
                self.range(template.Position + 1, toks.End)
                .First(self.NonWhitespace)
                .Assert("Invalid template declaration", lambda t: t.Value == "<")
                .GetMatchingRangeIn(toks)
            )
            actor.templateFormals = [
                self.ParseVarDeclaration(p)
                for p in self.SplitParameterList(template_params, ",")
            ]
            toks = self.range(template_params.End + 1, toks.End)

        attribute = toks.First(self.NonWhitespace)
        while attribute.Value == "[":
            attribute_contents = attribute.GetMatchingRangeIn(toks)
            as_array = list(attribute_contents)
            if (
                len(as_array) < 2
                or as_array[0].Value != "["
                or as_array[-1].Value != "]"
            ):
                raise Error(actor.SourceLine, "Invalid attribute: Expected [[...]]")
            actor.attributes.append(
                "["
                + self.str_tokens(self.NormalizeWhitespace(attribute_contents))
                + "]"
            )
            toks = self.range(attribute_contents.End + 1, toks.End)
            attribute = toks.First(self.NonWhitespace)

        static_keyword = toks.First(self.NonWhitespace)
        if static_keyword.Value == "static":
            actor.isStatic = True
            toks = self.range(static_keyword.Position + 1, toks.End)

        uncancellable_keyword = toks.First(self.NonWhitespace)
        if uncancellable_keyword.Value == "UNCANCELLABLE":
            actor.SetUncancellable()
            toks = self.range(uncancellable_keyword.Position + 1, toks.End)

        param_range = (
            toks.Last(self.NonWhitespace)
            .Assert(
                "Unexpected tokens after actor parameter list.",
                lambda t: t.Value == ")" and t.ParenDepth == toks.First().ParenDepth,
            )
            .GetMatchingRangeIn(toks)
        )
        actor.parameters = [
            self.ParseVarDeclaration(p)
            for p in self.SplitParameterList(param_range, ",")
        ]

        name = self.range(toks.Begin, param_range.Begin - 1).Last(self.NonWhitespace)
        actor.name = name.Value

        return_type_range = self.range(
            toks.First().Position + 1, name.Position
        ).SkipWhile(self.Whitespace)
        ret_token = return_type_range.First()
        if ret_token.Value == "Future":
            of_type = (
                return_type_range.Skip(1)
                .First(self.NonWhitespace)
                .Assert("Expected <", lambda tok: tok.Value == "<")
                .GetMatchingRangeIn(return_type_range)
            )
            actor.returnType = self.str_tokens(self.NormalizeWhitespace(of_type))
            toks = self.range(of_type.End + 1, return_type_range.End)
        elif ret_token.Value == "void":
            actor.returnType = None
            toks = return_type_range.Skip(1)
        else:
            raise Error(actor.SourceLine, "Actor apparently does not return Future<T>")

        toks = toks.SkipWhile(self.Whitespace)
        if not toks.IsEmpty:
            if toks.Last().Value == "::":
                actor.nameSpace = self.str_tokens(self.range(toks.Begin, toks.End - 1))
            else:
                raise Error(
                    actor.SourceLine,
                    "Unrecognized tokens preceding parameter list in actor declaration",
                )

        if (
            self.errorMessagePolicy.ActorsNoDiscardByDefault()
            and "[[flow_allow_discard]]" not in actor.attributes
        ):
            if actor.IsCancellable():
                actor.attributes.append("[[nodiscard]]")
        known_flow_attributes = {"[[flow_allow_discard]]"}
        for flow_attr in [a for a in actor.attributes if a.startswith("[[flow_")]:
            if flow_attr not in known_flow_attributes:
                raise Error(actor.SourceLine, f"Unknown flow attribute {flow_attr}")
        actor.attributes = [a for a in actor.attributes if not a.startswith("[[flow_")]

    def ParseClassContext(self, toks: TokenRange) -> Tuple[bool, str]:
        name = ""
        if toks.Begin == toks.End:
            return False, name
        while True:
            first = toks.First(self.NonWhitespace)
            if first.Value == "[":
                contents = first.GetMatchingRangeIn(toks)
                toks = self.range(contents.End + 1, toks.End)
            elif first.Value == "alignas":
                toks = self.range(first.Position + 1, toks.End)
                first = toks.First(self.NonWhitespace)
                first.Assert("Expected ( after alignas", lambda t: t.Value == "(")
                contents = first.GetMatchingRangeIn(toks)
                toks = self.range(contents.End + 1, toks.End)
            else:
                break
        first = toks.First(self.NonWhitespace)
        if not self.identifierPattern.match(first.Value):
            return False, name
        while True:
            first.Assert(
                "Expected identifier",
                lambda t: bool(self.identifierPattern.match(t.Value)),
            )
            name += first.Value
            toks = self.range(first.Position + 1, toks.End)
            if toks.First(self.NonWhitespace).Value == "::":
                name += "::"
                toks = toks.SkipWhile(self.Whitespace).Skip(1)
            else:
                break
            first = toks.First(self.NonWhitespace)
        toks = toks.SkipWhile(
            lambda t: self.Whitespace(t) or t.Value in {"final", "explicit"}
        )
        first = toks.First(self.NonWhitespace)
        if first.Value in {":", "{"}:
            return True, name
        return False, name

    def ParseLoopStatement(self, toks: TokenRange) -> LoopStatement:
        return LoopStatement(body=self.ParseCompoundStatement(toks.Consume("loop")))

    def ParseChooseStatement(self, toks: TokenRange) -> ChooseStatement:
        return ChooseStatement(body=self.ParseCompoundStatement(toks.Consume("choose")))

    def ParseWhenStatement(self, toks: TokenRange) -> WhenStatement:
        expr = (
            toks.Consume("when")
            .SkipWhile(self.Whitespace)
            .First()
            .Assert("Expected (", lambda t: t.Value == "(")
            .GetMatchingRangeIn(toks)
            .SkipWhile(self.Whitespace)
        )
        return WhenStatement(
            wait=self.ParseWaitStatement(expr),
            body=self.ParseCompoundStatement(self.range(expr.End + 1, toks.End)),
        )

    def ParseStateDeclaration(self, toks: TokenRange) -> StateDeclarationStatement:
        toks = toks.Consume("state").RevSkipWhile(lambda t: t.Value == ";")
        return StateDeclarationStatement(decl=self.ParseVarDeclaration(toks))

    def ParseReturnStatement(self, toks: TokenRange) -> ReturnStatement:
        toks = toks.Consume("return").RevSkipWhile(lambda t: t.Value == ";")
        return ReturnStatement(
            expression=self.str_tokens(self.NormalizeWhitespace(toks))
        )

    def ParseThrowStatement(self, toks: TokenRange) -> ThrowStatement:
        toks = toks.Consume("throw").RevSkipWhile(lambda t: t.Value == ";")
        return ThrowStatement(
            expression=self.str_tokens(self.NormalizeWhitespace(toks))
        )

    def ParseWaitStatement(self, toks: TokenRange) -> WaitStatement:
        ws = WaitStatement(
            result=VarDeclaration(
                type="Void",
                name="_",
                initializer="",
                initializerConstructorSyntax=False,
            ),
            futureExpression="",
            resultIsState=False,
            isWaitNext=False,
            FirstSourceLine=toks.First().SourceLine,
        )
        if toks.First().Value == "state":
            ws.resultIsState = True
            toks = toks.Consume("state")
        initializer: Optional[TokenRange]
        if toks.First().Value in {"wait", "waitNext"}:
            initializer = toks.RevSkipWhile(lambda t: t.Value == ";")
            ws.result = VarDeclaration(
                name="_",
                type="Void",
                initializer="",
                initializerConstructorSyntax=False,
            )
        else:
            name_tok, typ_range, initializer, constructor = self.ParseDeclaration(
                toks.RevSkipWhile(lambda t: t.Value == ";")
            )
            typestring = self.str_tokens(self.NormalizeWhitespace(typ_range))
            if typestring == "Void":
                raise Error(
                    ws.FirstSourceLine,
                    "Assigning the result of a Void wait is not allowed.  Just use a standalone wait statement.",
                )
            ws.result = VarDeclaration(
                name=name_tok.Value,
                type=typestring,
                initializer="",
                initializerConstructorSyntax=False,
            )
        if initializer is None:
            raise Error(
                ws.FirstSourceLine,
                "Wait statement must be a declaration or standalone statement",
            )

        def _wait_pred(token: Token) -> bool:
            if token.Value == "waitNext":
                ws.isWaitNext = True
                return True
            return token.Value == "wait"

        wait_params = (
            initializer.SkipWhile(self.Whitespace)
            .Consume(
                "Statement contains a wait, but is not a valid wait statement or a supported compound statement.1",
                _wait_pred,
            )
            .SkipWhile(self.Whitespace)
            .First()
            .Assert("Expected (", lambda t: t.Value == "(")
            .GetMatchingRangeIn(initializer)
        )
        if not all(
            self.Whitespace(t)
            for t in self.range(wait_params.End, initializer.End).Consume(")")
        ):
            raise Error(
                toks.First().SourceLine,
                "Statement contains a wait, but is not a valid wait statement or a supported compound statement.2",
            )

        ws.futureExpression = self.str_tokens(self.NormalizeWhitespace(wait_params))
        return ws

    def ParseWhileStatement(self, toks: TokenRange) -> WhileStatement:
        expr = (
            toks.Consume("while")
            .First(self.NonWhitespace)
            .Assert("Expected (", lambda t: t.Value == "(")
            .GetMatchingRangeIn(toks)
        )
        return WhileStatement(
            expression=self.str_tokens(self.NormalizeWhitespace(expr)),
            body=self.ParseCompoundStatement(self.range(expr.End + 1, toks.End)),
        )

    def ParseForStatement(self, toks: TokenRange) -> Statement:
        head = (
            toks.Consume("for")
            .First(self.NonWhitespace)
            .Assert("Expected (", lambda t: t.Value == "(")
            .GetMatchingRangeIn(toks)
        )
        delim = [
            t
            for t in head
            if t.ParenDepth == head.First().ParenDepth
            and t.BraceDepth == head.First().BraceDepth
            and t.Value == ";"
        ]
        if len(delim) == 2:
            init = self.range(head.Begin, delim[0].Position)
            cond = self.range(delim[0].Position + 1, delim[1].Position)
            nxt = self.range(delim[1].Position + 1, head.End)
            body = self.range(head.End + 1, toks.End)
            return ForStatement(
                initExpression=self.str_tokens(self.NormalizeWhitespace(init)),
                condExpression=self.str_tokens(self.NormalizeWhitespace(cond)),
                nextExpression=self.str_tokens(self.NormalizeWhitespace(nxt)),
                body=self.ParseCompoundStatement(body),
            )

        delim = [
            t
            for t in head
            if t.ParenDepth == head.First().ParenDepth
            and t.BraceDepth == head.First().BraceDepth
            and t.Value == ":"
        ]
        if len(delim) != 1:
            raise Error(
                head.First().SourceLine,
                "for statement must be 3-arg style or c++11 2-arg style",
            )

        return RangeForStatement(
            rangeExpression=self.str_tokens(
                self.NormalizeWhitespace(
                    self.range(delim[0].Position + 1, head.End).SkipWhile(
                        self.Whitespace
                    )
                )
            ),
            rangeDecl=self.str_tokens(
                self.NormalizeWhitespace(
                    self.range(head.Begin, delim[0].Position - 1).SkipWhile(
                        self.Whitespace
                    )
                )
            ),
            body=self.ParseCompoundStatement(self.range(head.End + 1, toks.End)),
        )

    def ParseIfStatement(self, toks: TokenRange) -> IfStatement:
        toks = toks.Consume("if").SkipWhile(self.Whitespace)
        constexpr = toks.First().Value == "constexpr"
        if constexpr:
            toks = toks.Consume("constexpr").SkipWhile(self.Whitespace)
        expr = (
            toks.First(self.NonWhitespace)
            .Assert("Expected (", lambda t: t.Value == "(")
            .GetMatchingRangeIn(toks)
        )
        return IfStatement(
            expression=self.str_tokens(self.NormalizeWhitespace(expr)),
            constexpr=constexpr,
            ifBody=self.ParseCompoundStatement(self.range(expr.End + 1, toks.End)),
            elseBody=None,
        )

    def ParseElseStatement(self, toks: TokenRange, prev_statement: Statement) -> None:
        if_statement = (
            prev_statement if isinstance(prev_statement, IfStatement) else None
        )
        while (
            isinstance(if_statement, IfStatement) and if_statement.elseBody is not None
        ):
            if_statement = (
                if_statement.elseBody
                if isinstance(if_statement.elseBody, IfStatement)
                else None
            )
        if if_statement is None:
            raise Error(toks.First().SourceLine, "else without matching if")
        if_statement.elseBody = self.ParseCompoundStatement(toks.Consume("else"))

    def ParseTryStatement(self, toks: TokenRange) -> TryStatement:
        return TryStatement(
            tryBody=self.ParseCompoundStatement(toks.Consume("try")), catches=[]
        )

    def ParseCatchStatement(self, toks: TokenRange, prev_statement: Statement) -> None:
        try_statement = (
            prev_statement if isinstance(prev_statement, TryStatement) else None
        )
        if try_statement is None:
            raise Error(toks.First().SourceLine, "catch without matching try")
        expr = (
            toks.Consume("catch")
            .First(self.NonWhitespace)
            .Assert("Expected (", lambda t: t.Value == "(")
            .GetMatchingRangeIn(toks)
        )
        try_statement.catches.append(
            TryStatement.Catch(
                expression=self.str_tokens(self.NormalizeWhitespace(expr)),
                body=self.ParseCompoundStatement(self.range(expr.End + 1, toks.End)),
                FirstSourceLine=expr.First().SourceLine,
            )
        )

    IllegalKeywords = {"goto", "do", "finally", "__if_exists", "__if_not_exists"}

    def ParseDeclarationDescr(
        self, toks: TokenRange, declarations: List[Declaration]
    ) -> None:
        delim = toks.First(lambda t: t.Value == ";")
        name_range = (
            self.range(toks.Begin, delim.Position)
            .RevSkipWhile(self.Whitespace)
            .RevTakeWhile(self.NonWhitespace)
        )
        type_range = self.range(toks.Begin, name_range.Begin)
        comment_range = self.range(delim.Position + 1, toks.End)
        declarations.append(
            Declaration(
                name=self.str_tokens(name_range).strip(),
                type=self.str_tokens(type_range).strip(),
                comment=self.str_tokens(comment_range).strip().lstrip("/"),
            )
        )

    def ParseStatement(self, toks: TokenRange, statements: List[Statement]) -> None:
        toks = toks.SkipWhile(self.Whitespace)

        def Add(stmt: Statement) -> None:
            stmt.FirstSourceLine = toks.First().SourceLine
            statements.append(stmt)

        first_val = toks.First().Value
        if first_val == "loop":
            Add(self.ParseLoopStatement(toks))
        elif first_val == "while":
            Add(self.ParseWhileStatement(toks))
        elif first_val == "for":
            Add(self.ParseForStatement(toks))
        elif first_val == "break":
            Add(BreakStatement())
        elif first_val == "continue":
            Add(ContinueStatement())
        elif first_val == "return":
            Add(self.ParseReturnStatement(toks))
        elif first_val == "{":
            Add(self.ParseCompoundStatement(toks))
        elif first_val == "if":
            Add(self.ParseIfStatement(toks))
        elif first_val == "else":
            self.ParseElseStatement(toks, statements[-1])
        elif first_val == "choose":
            Add(self.ParseChooseStatement(toks))
        elif first_val == "when":
            Add(self.ParseWhenStatement(toks))
        elif first_val == "try":
            Add(self.ParseTryStatement(toks))
        elif first_val == "catch":
            self.ParseCatchStatement(toks, statements[-1])
        elif first_val == "throw":
            Add(self.ParseThrowStatement(toks))
        else:
            if first_val in self.IllegalKeywords:
                raise Error(
                    toks.First().SourceLine,
                    f"Statement '{first_val}' not supported in actors.",
                )
            if any(t.Value in {"wait", "waitNext"} for t in toks):
                Add(self.ParseWaitStatement(toks))
            elif first_val == "state":
                Add(self.ParseStateDeclaration(toks))
            elif first_val == "switch" and any(t.Value == "return" for t in toks):
                raise Error(
                    toks.First().SourceLine,
                    "Unsupported compound statement containing return.",
                )
            elif first_val.startswith("#"):
                raise Error(
                    toks.First().SourceLine,
                    f'Found "{first_val}". Preprocessor directives are not supported within ACTORs',
                )
            elif any(
                self.NonWhitespace(t)
                for t in toks.RevSkipWhile(lambda t: t.Value == ";")
            ):
                Add(
                    PlainOldCodeStatement(
                        code=self.str_tokens(
                            self.NormalizeWhitespace(
                                toks.RevSkipWhile(lambda t: t.Value == ";")
                            )
                        )
                        + ";"
                    )
                )

    def ParseCompoundStatement(self, toks: TokenRange) -> Statement:
        first = toks.First(self.NonWhitespace)
        if first.Value == "{":
            in_braces = first.GetMatchingRangeIn(toks)
            if not all(
                self.Whitespace(t)
                for t in self.range(in_braces.End, toks.End).Consume("}")
            ):
                raise Error(
                    in_braces.Last().SourceLine,
                    "Unexpected tokens after compound statement",
                )
            return self.ParseCodeBlock(in_braces)
        else:
            statements: List[Statement] = []
            self.ParseStatement(toks.Skip(1), statements)
            return statements[0]

    def ParseDescrCodeBlock(self, toks: TokenRange) -> List[Declaration]:
        declarations: List[Declaration] = []
        while True:
            delim = next((t for t in toks if t.Value == ";"), None)
            if delim is None:
                break
            pos = delim.Position + 1
            potential_comment = self.range(pos, toks.End).SkipWhile(
                lambda t: t.Value in {"\t", " "}
            )
            if (
                not potential_comment.IsEmpty
                and potential_comment.First().Value.startswith("//")
            ):
                pos = potential_comment.First().Position + 1
            self.ParseDeclarationDescr(self.range(toks.Begin, pos), declarations)
            toks = self.range(pos, toks.End)
        if not all(self.Whitespace(t) for t in toks):
            raise Error(
                toks.First(self.NonWhitespace).SourceLine,
                "Trailing unterminated statement in code block",
            )
        return declarations

    def ParseCodeBlock(self, toks: TokenRange) -> CodeBlock:
        statements: List[Statement] = []
        while True:
            delim = next(
                (
                    t
                    for t in toks
                    if t.ParenDepth == toks.First().ParenDepth
                    and t.BraceDepth == toks.First().BraceDepth
                    and (t.Value == ";" or t.Value == "}")
                ),
                None,
            )
            if delim is None:
                break
            self.ParseStatement(self.range(toks.Begin, delim.Position + 1), statements)
            toks = self.range(delim.Position + 1, toks.End)
        if not all(self.Whitespace(t) for t in toks):
            raise Error(
                toks.First(self.NonWhitespace).SourceLine,
                "Trailing unterminated statement in code block",
            )
        return CodeBlock(statements=statements)

    def ParseDescr(self, pos: int) -> Tuple[Descr, int]:
        descr = Descr()
        toks = self.range(pos + 1, len(self.tokens))
        heading = toks.TakeWhile(lambda t: t.Value != "{")
        body = self.range(heading.End + 1, len(self.tokens)).TakeWhile(
            lambda t: t.BraceDepth > toks.First().BraceDepth or t.Value == ";"
        )
        self.ParseDescrHeading(descr, heading)
        descr.body = self.ParseDescrCodeBlock(body)
        end = body.End + 1
        return descr, end

    def ParseActor(self, pos: int) -> Tuple[Actor, int]:
        actor = Actor()
        head_token = self.tokens[pos]
        actor.SourceLine = head_token.SourceLine

        toks = self.range(pos + 1, len(self.tokens))
        heading = toks.TakeWhile(lambda t: t.Value != "{")
        to_semicolon = toks.TakeWhile(lambda t: t.Value != ";")
        actor.isForwardDeclaration = (
            heading.Length > 0 and to_semicolon.Length < heading.Length
        )
        if actor.isForwardDeclaration:
            heading = to_semicolon
            if head_token.Value == "ACTOR":
                self.ParseActorHeading(actor, heading)
            else:
                head_token.Assert("ACTOR expected!", lambda _: False)
            end = heading.End + 1
            return actor, end

        body = self.range(heading.End + 1, len(self.tokens)).TakeWhile(
            lambda t: t.BraceDepth > toks.First().BraceDepth
        )
        if head_token.Value == "ACTOR":
            self.ParseActorHeading(actor, heading)
        elif head_token.Value == "TEST_CASE":
            self.ParseTestCaseHeading(actor, heading)
            actor.isTestCase = True
        else:
            head_token.Assert("ACTOR or TEST_CASE expected!", lambda _: False)

        actor.body = self.ParseCodeBlock(body)
        if not actor.body.containsWait():
            self.errorMessagePolicy.HandleActorWithoutWait(self.sourceFile, actor)
        end = body.End + 1
        return actor, end

    def str_range(self, begin: int, end: int) -> str:
        return self.str_tokens(self.range(begin, end))

    def CountParens(self) -> None:
        brace_depth = 0
        paren_depth = 0
        line_count = 1
        last_paren: Optional[Token] = None
        last_brace: Optional[Token] = None
        for i, tok in enumerate(self.tokens):
            if tok.Value == "}":
                brace_depth -= 1
            elif tok.Value == ")":
                paren_depth -= 1
            elif tok.Value == "\r\n":
                line_count += 1
            elif tok.Value == "\n":
                line_count += 1
            if brace_depth < 0:
                raise Error(line_count, "Mismatched braces")
            if paren_depth < 0:
                raise Error(line_count, "Mismatched parenthesis")
            tok.Position = i
            tok.SourceLine = line_count
            tok.BraceDepth = brace_depth
            tok.ParenDepth = paren_depth
            if tok.Value.startswith("/*"):
                line_count += tok.Value.count("\n")
            if tok.Value == "{":
                brace_depth += 1
                if brace_depth == 1:
                    last_brace = tok
            elif tok.Value == "(":
                paren_depth += 1
                if paren_depth == 1:
                    last_paren = tok
        if brace_depth != 0:
            raise Error(
                last_brace.SourceLine if last_brace else line_count, "Unmatched brace"
            )
        if paren_depth != 0:
            raise Error(
                last_paren.SourceLine if last_paren else line_count,
                "Unmatched parenthesis",
            )

    def Tokenize(self, text: str) -> Iterator[str]:
        pos = 0
        while pos < len(text):
            matched = False
            for regex in self.tokenExpressions:
                m = regex.match(text, pos)
                if m:
                    yield m.group(0)
                    pos += len(m.group(0))
                    matched = True
                    break
            if not matched:
                raise Exception(f"Can't tokenize! {pos}")

    def Write(self, writer: io.TextIOBase, destFileName: str) -> None:
        writer.write("#define POST_ACTOR_COMPILER 1\n")
        outLine = 1
        if self.LineNumbersEnabled:
            writer.write(f'#line {self.tokens[0].SourceLine} "{self.sourceFile}"\n')
            outLine += 1
        inBlocks = 0
        classContextStack: List[ClassContext] = []
        i = 0
        while i < len(self.tokens):
            if self.tokens[0].SourceLine == 0:
                raise Exception("Internal error: Invalid source line (0)")
            tok = self.tokens[i]
            if tok.Value in {"ACTOR", "TEST_CASE"}:
                actor, end = self.ParseActor(i)
                if classContextStack:
                    actor.enclosingClass = "::".join(ctx.name for ctx in classContextStack)
                actor_writer = io.StringIO()
                actor_writer.write("")
                ActorCompiler(
                    actor,
                    self.sourceFile,
                    inBlocks == 0,
                    self.LineNumbersEnabled,
                    self.generateProbes,
                ).Write(actor_writer)
                actor_lines = actor_writer.getvalue().split("\n")

                hasLineNumber = False
                hadLineNumber = True
                for line in actor_lines:
                    if self.LineNumbersEnabled:
                        isLineNumber = "#line" in line
                        if isLineNumber:
                            hadLineNumber = True
                        if not isLineNumber and not hasLineNumber and hadLineNumber:
                            writer.write(
                                '\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t#line {0} "{1}"\n'.format(
                                    outLine + 1, destFileName
                                )
                            )
                            outLine += 1
                            hadLineNumber = False
                        hasLineNumber = isLineNumber
                    writer.write(line.rstrip("\r\n"))
                    writer.write("\n")
                    outLine += 1
                i = end
                if i != len(self.tokens) and self.LineNumbersEnabled:
                    writer.write(
                        f'#line {self.tokens[i].SourceLine} "{self.sourceFile}"\n'
                    )
                    outLine += 1
            elif tok.Value == "DESCR":
                descr, end = self.ParseDescr(i)
                lines_out = DescrCompiler(descr, self.tokens[i].BraceDepth).Write(
                    writer
                )
                i = end
                outLine += lines_out
                if i != len(self.tokens) and self.LineNumbersEnabled:
                    writer.write(
                        f'#line {self.tokens[i].SourceLine} "{self.sourceFile}"\n'
                    )
                    outLine += 1
            elif tok.Value in {"class", "struct", "union"}:
                writer.write(tok.Value)
                success, name = self.ParseClassContext(
                    self.range(i + 1, len(self.tokens))
                )
                if success:
                    classContextStack.append(ClassContext(name=name, inBlocks=inBlocks))
            else:
                if tok.Value == "{":
                    inBlocks += 1
                elif tok.Value == "}":
                    inBlocks -= 1
                    if classContextStack and classContextStack[-1].inBlocks == inBlocks:
                        classContextStack.pop()
                writer.write(tok.Value)
                outLine += tok.Value.count("\n")
            i += 1


class TypeSwitch:
    def __init__(self, value):
        self.value = value
        self.result = None
        self.ok = False

    def Case(self, typ, func: Callable):
        if not self.ok and isinstance(self.value, typ):
            self.result = func(self.value)
            self.ok = True
        return self

    def Return(self):
        if not self.ok:
            raise Exception("Typeswitch didn't match.")
        return self.result


class Context:
    def __init__(
        self,
        target: Optional["Function"] = None,
        next: Optional["Function"] = None,
        breakF: Optional["Function"] = None,
        continueF: Optional["Function"] = None,
        catchFErr: Optional["Function"] = None,
        tryLoopDepth: int = 0,
    ) -> None:
        self.target = target
        self.next = next
        self.breakF = breakF
        self.continueF = continueF
        self.catchFErr = catchFErr
        self.tryLoopDepth = tryLoopDepth

    def unreachable(self) -> None:
        self.target = None

    def WithTarget(self, newTarget: "Function") -> "Context":
        return Context(
            target=newTarget,
            breakF=self.breakF,
            continueF=self.continueF,
            next=None,
            catchFErr=self.catchFErr,
            tryLoopDepth=self.tryLoopDepth,
        )

    def LoopContext(
        self,
        newTarget: "Function",
        breakF: "Function",
        continueF: "Function",
        deltaLoopDepth: int,
    ) -> "Context":
        return Context(
            target=newTarget,
            breakF=breakF,
            continueF=continueF,
            next=None,
            catchFErr=self.catchFErr,
            tryLoopDepth=self.tryLoopDepth + deltaLoopDepth,
        )

    def WithCatch(self, newCatchFErr: "Function") -> "Context":
        return Context(
            target=self.target,
            breakF=self.breakF,
            continueF=self.continueF,
            next=None,
            catchFErr=newCatchFErr,
            tryLoopDepth=0,
        )

    def Clone(self) -> "Context":
        return Context(
            target=self.target,
            next=self.next,
            breakF=self.breakF,
            continueF=self.continueF,
            catchFErr=self.catchFErr,
            tryLoopDepth=self.tryLoopDepth,
        )


class Function:
    def __init__(self) -> None:
        self.name: str = ""
        self.returnType: str = ""
        self.formalParameters: Sequence[str] = []
        self.endIsUnreachable: bool = False
        self.exceptionParameterIs: Optional[str] = None
        self.publicName: bool = False
        self.specifiers: str = ""
        self.indentation: str = ""
        self.body: io.StringIO = io.StringIO()
        self.wasCalled: bool = False
        self.overload: Optional["Function"] = None

    def setOverload(self, overload: "Function") -> None:
        self.overload = overload

    def popOverload(self) -> Optional["Function"]:
        result = self.overload
        self.overload = None
        return result

    def addOverload(self, *formalParameters: str) -> None:
        overload = Function()
        overload.name = self.name
        overload.returnType = self.returnType
        overload.endIsUnreachable = self.endIsUnreachable
        overload.formalParameters = list(formalParameters)
        overload.indentation = self.indentation
        self.setOverload(overload)

    def Indent(self, change: int) -> None:
        if change > 0:
            self.indentation += "\t" * change
        elif change < 0:
            self.indentation = self.indentation[:change]
        if self.overload:
            self.overload.Indent(change)

    def WriteLineUnindented(self, s: str) -> None:
        self.body.write(s + "\n")
        if self.overload:
            self.overload.WriteLineUnindented(s)

    def WriteLine(self, line: str, *args) -> None:
        formatted = line.format(*args) if args else line
        self.body.write(self.indentation + formatted + "\n")
        if self.overload:
            self.overload.WriteLine(line, *args)

    @property
    def BodyText(self) -> str:
        return self.body.getvalue()

    def useByName(self) -> str:
        self.wasCalled = True
        return self.name if self.publicName else f"a_{self.name}"

    def call(self, *parameters: str) -> str:
        return f"{self.useByName()}({', '.join(parameters)})"


class LiteralBreak(Function):
    def __init__(self) -> None:
        super().__init__()
        self.name = "break!"

    def call(self, *parameters: str) -> str:
        self.wasCalled = True
        if parameters:
            raise Exception("LiteralBreak called with parameters!")
        return "break"


class LiteralContinue(Function):
    def __init__(self) -> None:
        super().__init__()
        self.name = "continue!"

    def call(self, *parameters: str) -> str:
        self.wasCalled = True
        if parameters:
            raise Exception("LiteralContinue called with parameters!")
        return "continue"


class StateVar(VarDeclaration):
    def __init__(self, SourceLine: int = 0, **kwargs):
        super().__init__(**kwargs)
        self.SourceLine: int = SourceLine


class CallbackVar(StateVar):
    def __init__(self, CallbackGroup: int = 0, **kwargs):
        super().__init__(**kwargs)
        self.CallbackGroup: int = CallbackGroup


class DescrCompiler:
    def __init__(self, descr: Descr, braceDepth: int) -> None:
        self.descr = descr
        self.memberIndentStr = "\t" * braceDepth

    def Write(self, writer: io.TextIOBase) -> int:
        lines = 0
        writer.write(
            f"{self.memberIndentStr}template<> struct Descriptor<struct {self.descr.name}> {{\n"
        )
        writer.write(
            f'{self.memberIndentStr}\tstatic StringRef typeName() {{ return "{self.descr.name}"_sr; }}\n'
        )
        writer.write(f"{self.memberIndentStr}\ttypedef {self.descr.name} type;\n")
        lines += 3

        for dec in self.descr.body:
            writer.write(f"{self.memberIndentStr}\tstruct {dec.name}Descriptor {{\n")
            writer.write(
                f'{self.memberIndentStr}\t\tstatic StringRef name() {{ return "{dec.name}"_sr; }}\n'
            )
            writer.write(
                f'{self.memberIndentStr}\t\tstatic StringRef typeName() {{ return "{dec.type}"_sr; }}\n'
            )
            writer.write(
                f'{self.memberIndentStr}\t\tstatic StringRef comment() {{ return "{dec.comment}"_sr; }}\n'
            )
            writer.write(f"{self.memberIndentStr}\t\ttypedef {dec.type} type;\n")
            writer.write(
                f"{self.memberIndentStr}\t\tstatic inline type get({self.descr.name}& from);\n"
            )
            writer.write(f"{self.memberIndentStr}\t}};\n")
            lines += 7

        writer.write(f"{self.memberIndentStr}\ttypedef std::tuple<")
        first_desc = True
        for dec in self.descr.body:
            if not first_desc:
                writer.write(",")
            writer.write(f"{dec.name}Descriptor")
            first_desc = False
        writer.write("> fields;\n")
        writer.write(
            f"{self.memberIndentStr}\ttypedef make_index_sequence_impl<0, index_sequence<>, std::tuple_size<fields>::value>::type field_indexes;\n"
        )
        writer.write(f"{self.memberIndentStr}}};\n")
        if self.descr.superClassList:
            writer.write(
                f"{self.memberIndentStr}struct {self.descr.name} : {self.descr.superClassList} {{\n"
            )
        else:
            writer.write(f"{self.memberIndentStr}struct {self.descr.name} {{\n")
        lines += 4

        for dec in self.descr.body:
            writer.write(
                f"{self.memberIndentStr}\t{dec.type} {dec.name}; //{dec.comment}\n"
            )
            lines += 1

        writer.write(f"{self.memberIndentStr}}};\n")
        lines += 1

        for dec in self.descr.body:
            writer.write(
                f"{self.memberIndentStr}{dec.type} Descriptor<{self.descr.name}>::{dec.name}Descriptor::get({self.descr.name}& from) {{ return from.{dec.name}; }}\n"
            )
            lines += 1
        return lines


class ActorCompiler:
    loopDepth0 = "int loopDepth=0"
    loopDepth = "int loopDepth"
    codeIndent = 2
    memberIndentStr = "\t"
    usedClassNames: set = set()

    def __init__(
        self,
        actor: Actor,
        sourceFile: str,
        isTopLevel: bool,
        lineNumbersEnabled: bool,
        generateProbes: bool,
    ) -> None:
        self.actor = actor
        self.sourceFile = sourceFile
        self.state: List[StateVar] = []
        self.callbacks: List[CallbackVar] = []
        self.isTopLevel = isTopLevel
        self.LineNumbersEnabled = lineNumbersEnabled
        self.chooseGroups = 0
        self.whenCount = 0
        self.This = ""
        self.generateProbes = generateProbes
        self.functions: dict[str, Function] = {}
        self.iterators: dict[str, int] = {}
        self.className = ""
        self.fullClassName = ""
        self.stateClassName = ""
        self.FindState()

    def Write(self, writer: io.TextIOBase) -> None:
        fullReturnType = (
            f"Future<{self.actor.returnType}>"
            if self.actor.returnType is not None
            else "void"
        )
        i = 0
        while True:
            self.className = "{3}{0}{1}Actor{2}".format(
                self.actor.name[0].upper(),
                self.actor.name[1:],
                str(i) if i != 0 else "",
                (
                    self.actor.enclosingClass.replace("::", "_") + "_"
                    if self.actor.enclosingClass is not None
                    and self.actor.isForwardDeclaration
                    else self.actor.nameSpace.replace("::", "_") + "_"
                    if self.actor.nameSpace is not None
                    else ""
                ),
            )
            if self.actor.isForwardDeclaration:
                break
            if self.className not in ActorCompiler.usedClassNames:
                ActorCompiler.usedClassNames.add(self.className)
                break
            i += 1

        self.fullClassName = self.className + self.GetTemplateActuals()
        actorClassFormal = VarDeclaration(
            name=self.className,
            type="class",
            initializer="",
            initializerConstructorSyntax=False,
        )
        self.This = f"static_cast<{actorClassFormal.name}*>(this)"
        self.stateClassName = self.className + "State"
        fullStateClassName = self.stateClassName + self.GetTemplateActuals(
            VarDeclaration(
                type="class",
                name=self.fullClassName,
                initializer="",
                initializerConstructorSyntax=False,
            )
        )

        if self.actor.isForwardDeclaration:
            for attribute in self.actor.attributes:
                writer.write(attribute + " ")
            if self.actor.isStatic:
                writer.write("static ")
            writer.write(
                "{0} {3}{1}( {2} );\n".format(
                    fullReturnType,
                    self.actor.name,
                    ", ".join(self.ParameterList()),
                    "" if self.actor.nameSpace is None else self.actor.nameSpace + "::",
                )
            )
            if self.actor.enclosingClass is not None:
                writer.write(f"template <class> friend class {self.stateClassName};\n")
            return

        body = self.getFunction("", "body", [ActorCompiler.loopDepth0])
        bodyContext = Context(
            target=body,
            catchFErr=self.getFunction(
                body.name, "Catch", ["Error error", ActorCompiler.loopDepth0]
            ),
        )

        endContext = self.TryCatchCompile(self.actor.body, bodyContext)
        if endContext.target is not None:
            if self.actor.returnType is None:
                stmt = ReturnStatement(expression="")
                stmt.FirstSourceLine = self.actor.SourceLine
                self.CompileStatement(stmt, endContext)
            else:
                raise Error(
                    self.actor.SourceLine,
                    "Actor {0} fails to return a value",
                    self.actor.name,
                )

        if self.actor.returnType is not None:
            bodyContext.catchFErr.WriteLine("this->~{0}();", self.stateClassName)
            bodyContext.catchFErr.WriteLine(
                "{0}->sendErrorAndDelPromiseRef(error);", self.This
            )
        else:
            bodyContext.catchFErr.WriteLine("delete {0};", self.This)
        bodyContext.catchFErr.WriteLine("loopDepth = 0;")

        if self.isTopLevel and self.actor.nameSpace is None:
            writer.write("namespace {\n")

        writer.write(
            f"// This generated class is to be used only via {self.actor.name}()\n"
        )
        self.WriteTemplate(writer, actorClassFormal)
        self.LineNumber(writer, self.actor.SourceLine)
        writer.write(f"class {self.stateClassName} {{\n")
        writer.write("public:\n")
        self.LineNumber(writer, self.actor.SourceLine)
        self.WriteStateConstructor(writer)
        self.WriteStateDestructor(writer)
        self.WriteFunctions(writer)
        for st in self.state:
            self.LineNumber(writer, st.SourceLine)
            writer.write(f"\t{st.type} {st.name};\n")
        writer.write("};\n")

        writer.write(
            f"// This generated class is to be used only via {self.actor.name}()\n"
        )
        self.WriteTemplate(writer)
        self.LineNumber(writer, self.actor.SourceLine)
        callback_base_classes = ", ".join(f"public {cb.type}" for cb in self.callbacks)
        if callback_base_classes != "":
            callback_base_classes += ", "
        writer.write(
            "class {0} final : public Actor<{2}>, {3}public FastAllocated<{1}>, public {4} {{\n".format(
                self.className,
                self.fullClassName,
                "void" if self.actor.returnType is None else self.actor.returnType,
                callback_base_classes,
                fullStateClassName,
            )
        )
        writer.write("public:\n")
        writer.write(f"\tusing FastAllocated<{self.fullClassName}>::operator new;\n")
        writer.write(f"\tusing FastAllocated<{self.fullClassName}>::operator delete;\n")

        writer.write("#pragma clang diagnostic push\n")
        writer.write('#pragma clang diagnostic ignored "-Wdelete-non-virtual-dtor"\n')
        if self.actor.returnType is not None:
            writer.write(
                "\tvoid destroy() override {{ ((Actor<{0}>*)this)->~Actor(); operator delete(this); }}\n".format(
                    self.actor.returnType
                )
            )
        else:
            writer.write(
                "\tvoid destroy() {{ ((Actor<void>*)this)->~Actor(); operator delete(this); }}\n"
            )
        writer.write("#pragma clang diagnostic pop\n")

        for cb in self.callbacks:
            writer.write(f"friend struct {cb.type};\n")

        self.LineNumber(writer, self.actor.SourceLine)
        self.WriteConstructor(body, writer, fullStateClassName)
        self.WriteCancelFunc(writer)
        writer.write("};\n")
        if self.isTopLevel and self.actor.nameSpace is None:
            writer.write("}\n")
        self.WriteTemplate(writer)
        self.LineNumber(writer, self.actor.SourceLine)
        for attribute in self.actor.attributes:
            writer.write(attribute + " ")
        if self.actor.isStatic:
            writer.write("static ")
        writer.write(
            "{0} {3}{1}( {2} ) {{\n".format(
                fullReturnType,
                self.actor.name,
                ", ".join(self.ParameterList()),
                "" if self.actor.nameSpace is None else self.actor.nameSpace + "::",
            )
        )
        self.LineNumber(writer, self.actor.SourceLine)
        newActor = f"new {self.fullClassName}({', '.join(p.name for p in self.actor.parameters)})"
        if self.actor.returnType is not None:
            writer.write(
                "\treturn Future<{1}>({0});\n".format(newActor, self.actor.returnType)
            )
        else:
            writer.write(f"\t{newActor};\n")
        writer.write("}\n")

        if self.actor.testCaseParameters is not None:
            writer.write(
                f"ACTOR_TEST_CASE({self.actor.name}, {self.actor.testCaseParameters})\n"
            )

        print(f"\tCompiled ACTOR {self.actor.name} (line {self.actor.SourceLine})")

    @property
    def thisAddress(self) -> str:
        return "reinterpret_cast<unsigned long>(this)"

    def ProbeEnter(self, fun: Function, name: str, index: int = -1) -> None:
        if self.generateProbes:
            fun.WriteLine(
                'fdb_probe_actor_enter("{0}", {1}, {2});', name, self.thisAddress, index
            )

    def ProbeExit(self, fun: Function, name: str, index: int = -1) -> None:
        if self.generateProbes:
            fun.WriteLine(
                'fdb_probe_actor_exit("{0}", {1}, {2});', name, self.thisAddress, index
            )

    def ProbeCreate(self, fun: Function, name: str) -> None:
        if self.generateProbes:
            fun.WriteLine('fdb_probe_actor_create("{0}", {1});', name, self.thisAddress)

    def ProbeDestroy(self, fun: Function, name: str) -> None:
        if self.generateProbes:
            fun.WriteLine(
                'fdb_probe_actor_destroy("{0}", {1});', name, self.thisAddress
            )

    def LineNumber(self, writer, SourceLine: int) -> None:
        if SourceLine == 0:
            raise Exception("Internal error: Invalid source line (0)")
        if self.LineNumbersEnabled:
            writer.write("\t" * 15 + f'#line {SourceLine} "{self.sourceFile}"\n')

    def LineNumberFunction(self, writer: Function, SourceLine: int) -> None:
        if SourceLine == 0:
            raise Exception("Internal error: Invalid source line (0)")
        if self.LineNumbersEnabled:
            writer.WriteLineUnindented(
                "\t" * 15 + f'#line {SourceLine} "{self.sourceFile}"'
            )

    def TryCatch(
        self,
        cx: Context,
        catchFErr: Optional[Function],
        catchLoopDepth: int,
        action: Callable[[], None],
        useLoopDepth: bool = True,
    ) -> None:
        if catchFErr is not None:
            cx.target.WriteLine("try {")
            cx.target.Indent(+1)
        action()
        if catchFErr is not None:
            cx.target.Indent(-1)
            cx.target.WriteLine("}")
            cx.target.WriteLine("catch (Error& error) {")
            if useLoopDepth:
                cx.target.WriteLine(
                    "\tloopDepth = {0};",
                    catchFErr.call("error", self.AdjustLoopDepth(catchLoopDepth)),
                )
            else:
                cx.target.WriteLine("\t{0};", catchFErr.call("error", "0"))
            cx.target.WriteLine("} catch (...) {")
            if useLoopDepth:
                cx.target.WriteLine(
                    "\tloopDepth = {0};",
                    catchFErr.call(
                        "unknown_error()", self.AdjustLoopDepth(catchLoopDepth)
                    ),
                )
            else:
                cx.target.WriteLine("\t{0};", catchFErr.call("unknown_error()", "0"))
            cx.target.WriteLine("}")

    def TryCatchCompile(self, block: CodeBlock, cx: Context) -> Context:
        def inner():
            nonlocal cx
            cx = self.Compile(block, cx, True)
            if cx.target is not None:
                nxt = self.getFunction(cx.target.name, "cont", ActorCompiler.loopDepth)
                cx.target.WriteLine("loopDepth = {0};", nxt.call("loopDepth"))
                cx.target = nxt
                cx.next = None

        self.TryCatch(cx, cx.catchFErr, cx.tryLoopDepth, inner)
        return cx

    def WriteTemplate(
        self, writer: io.TextIOBase, *extraParameters: VarDeclaration
    ) -> None:
        formals = list(self.actor.templateFormals or []) + list(extraParameters)
        if not formals:
            return
        self.LineNumber(writer, self.actor.SourceLine)
        writer.write(
            "template <{0}>\n".format(", ".join(f"{p.type} {p.name}" for p in formals))
        )

    def GetTemplateActuals(self, *extraParameters: VarDeclaration) -> str:
        formals = list(self.actor.templateFormals or []) + list(extraParameters)
        if not formals:
            return ""
        return "<" + ", ".join(p.name for p in formals) + ">"

    def WillContinue(self, stmt: Statement) -> bool:
        return any(
            isinstance(st, (ChooseStatement, WaitStatement, TryStatement))
            for st in self.Flatten(stmt)
        )

    def AsCodeBlock(self, statement: Statement) -> CodeBlock:
        if isinstance(statement, CodeBlock):
            return statement
        return CodeBlock(statements=[statement])

    def CompileStatement(self, stmt: Statement, cx: Context) -> None:
        if isinstance(stmt, PlainOldCodeStatement):
            self._compile_plain_code(stmt, cx)
        elif isinstance(stmt, StateDeclarationStatement):
            self._compile_state_declaration(stmt, cx)
        elif isinstance(stmt, ForStatement):
            self._compile_for(stmt, cx)
        elif isinstance(stmt, RangeForStatement):
            self._compile_range_for(stmt, cx)
        elif isinstance(stmt, WhileStatement):
            self._compile_while(stmt, cx)
        elif isinstance(stmt, LoopStatement):
            self._compile_loop(stmt, cx)
        elif isinstance(stmt, ChooseStatement):
            self._compile_choose(stmt, cx)
        elif isinstance(stmt, BreakStatement):
            self._compile_break(stmt, cx)
        elif isinstance(stmt, ContinueStatement):
            self._compile_continue(stmt, cx)
        elif isinstance(stmt, WaitStatement):
            self._compile_wait(stmt, cx)
        elif isinstance(stmt, CodeBlock):
            self._compile_codeblock(stmt, cx)
        elif isinstance(stmt, ReturnStatement):
            self._compile_return(stmt, cx)
        elif isinstance(stmt, IfStatement):
            self._compile_if(stmt, cx)
        elif isinstance(stmt, TryStatement):
            self._compile_try(stmt, cx)
        elif isinstance(stmt, ThrowStatement):
            self._compile_throw(stmt, cx)
        else:
            raise Error(
                stmt.FirstSourceLine,
                "Statement type {0} not supported yet.",
                type(stmt).__name__,
            )

    def _compile_plain_code(self, stmt: PlainOldCodeStatement, cx: Context) -> None:
        self.LineNumberFunction(cx.target, stmt.FirstSourceLine)
        cx.target.WriteLine(stmt.code)

    def _compile_state_declaration(
        self, stmt: StateDeclarationStatement, cx: Context
    ) -> None:
        prefix_states = []
        for s in self.actor.body.statements:
            if isinstance(s, StateDeclarationStatement):
                prefix_states.append(s)
            else:
                break
        if stmt in prefix_states:
            self.state.append(
                StateVar(
                    SourceLine=stmt.FirstSourceLine,
                    name=stmt.decl.name,
                    type=stmt.decl.type,
                    initializer=stmt.decl.initializer,
                    initializerConstructorSyntax=stmt.decl.initializerConstructorSyntax,
                )
            )
        else:
            self.state.append(
                StateVar(
                    SourceLine=stmt.FirstSourceLine,
                    name=stmt.decl.name,
                    type=stmt.decl.type,
                    initializer=None,
                    initializerConstructorSyntax=False,
                )
            )
            if stmt.decl.initializer is not None:
                self.LineNumberFunction(cx.target, stmt.FirstSourceLine)
                if (
                    stmt.decl.initializerConstructorSyntax
                    or stmt.decl.initializer == ""
                ):
                    cx.target.WriteLine(
                        "{0} = {1}({2});",
                        stmt.decl.name,
                        stmt.decl.type,
                        stmt.decl.initializer,
                    )
                else:
                    cx.target.WriteLine(
                        "{0} = {1};", stmt.decl.name, stmt.decl.initializer
                    )

    def EmitNativeLoop(
        self, sourceLine: int, head: str, body: Statement, cx: Context
    ) -> bool:
        self.LineNumberFunction(cx.target, sourceLine)
        cx.target.WriteLine(head + " {")
        cx.target.Indent(+1)
        literalBreak = LiteralBreak()
        self.Compile(
            self.AsCodeBlock(body),
            cx.LoopContext(cx.target, literalBreak, LiteralContinue(), 0),
            True,
        )
        cx.target.Indent(-1)
        cx.target.WriteLine("}")
        return not literalBreak.wasCalled

    def _compile_for(self, stmt: ForStatement, cx: Context) -> None:
        noCondition = stmt.condExpression in ("", "true", "1")
        if not self.WillContinue(stmt.body):
            if (
                self.EmitNativeLoop(
                    stmt.FirstSourceLine,
                    f"for({stmt.initExpression};{stmt.condExpression};{stmt.nextExpression})",
                    stmt.body,
                    cx,
                )
                and noCondition
            ):
                cx.unreachable()
        else:
            init_stmt = PlainOldCodeStatement(code=stmt.initExpression + ";")
            init_stmt.FirstSourceLine = stmt.FirstSourceLine
            self.CompileStatement(init_stmt, cx)

            if noCondition:
                fullBody = stmt.body
            else:
                br = BreakStatement()
                br.FirstSourceLine = stmt.FirstSourceLine
                if_stmt = IfStatement(
                    expression=f"!({stmt.condExpression})",
                    constexpr=False,
                    ifBody=br,
                    elseBody=None,
                )
                if_stmt.FirstSourceLine = stmt.FirstSourceLine
                fullBody = CodeBlock(
                    statements=[if_stmt] + list(self.AsCodeBlock(stmt.body).statements)
                )
                fullBody.FirstSourceLine = stmt.FirstSourceLine

            loopF = self.getFunction(
                cx.target.name, "loopHead", ActorCompiler.loopDepth
            )
            loopBody = self.getFunction(
                cx.target.name, "loopBody", ActorCompiler.loopDepth
            )
            breakF = self.getFunction(cx.target.name, "break", ActorCompiler.loopDepth)
            continueF = (
                loopF
                if stmt.nextExpression == ""
                else self.getFunction(
                    cx.target.name, "continue", ActorCompiler.loopDepth
                )
            )

            loopF.WriteLine("int oldLoopDepth = ++loopDepth;")
            loopF.WriteLine(
                "while (loopDepth == oldLoopDepth) loopDepth = {0};",
                loopBody.call("loopDepth"),
            )

            endLoop = self.Compile(
                self.AsCodeBlock(fullBody),
                cx.LoopContext(loopBody, breakF, continueF, +1),
                True,
            ).target
            if endLoop is not None and endLoop != loopBody:
                if stmt.nextExpression != "":
                    next_stmt = PlainOldCodeStatement(code=stmt.nextExpression + ";")
                    next_stmt.FirstSourceLine = stmt.FirstSourceLine
                    self.CompileStatement(next_stmt, cx.WithTarget(endLoop))
                endLoop.WriteLine("if (loopDepth == 0) return {0};", loopF.call("0"))

            cx.target.WriteLine("loopDepth = {0};", loopF.call("loopDepth"))

            if continueF != loopF and continueF.wasCalled:
                next_stmt = PlainOldCodeStatement(code=stmt.nextExpression + ";")
                next_stmt.FirstSourceLine = stmt.FirstSourceLine
                self.CompileStatement(next_stmt, cx.WithTarget(continueF))
                continueF.WriteLine("if (loopDepth == 0) return {0};", loopF.call("0"))

            if breakF.wasCalled:
                self.TryCatch(
                    cx.WithTarget(breakF),
                    cx.catchFErr,
                    cx.tryLoopDepth,
                    lambda: breakF.WriteLine("return {0};", cx.next.call("loopDepth")),
                )
            else:
                cx.unreachable()

    def _compile_range_for(self, stmt: RangeForStatement, cx: Context) -> None:
        if self.WillContinue(stmt.body):
            container = next(
                (s for s in self.state if s.name == stmt.rangeExpression), None
            )
            if container is None:
                raise Error(
                    stmt.FirstSourceLine,
                    "container of range-based for with continuation must be a state variable",
                )

            iter_name = self.getIteratorName(cx)
            self.state.append(
                StateVar(
                    SourceLine=stmt.FirstSourceLine,
                    name=iter_name,
                    type=f"decltype(std::begin(std::declval<{container.type}>()))",
                    initializer=None,
                    initializerConstructorSyntax=False,
                )
            )
            equivalent = ForStatement(
                initExpression=f"{iter_name} = std::begin({stmt.rangeExpression})",
                condExpression=f"{iter_name} != std::end({stmt.rangeExpression})",
                nextExpression=f"++{iter_name}",
            )
            equivalent.FirstSourceLine = stmt.FirstSourceLine
            equivalent.body = CodeBlock(
                statements=[
                    PlainOldCodeStatement(code=f"{stmt.rangeDecl} = *{iter_name};"),
                    stmt.body,
                ]
            )
            equivalent.body.FirstSourceLine = stmt.FirstSourceLine
            equivalent.body.statements[0].FirstSourceLine = stmt.FirstSourceLine
            self.CompileStatement(equivalent, cx)
        else:
            self.EmitNativeLoop(
                stmt.FirstSourceLine,
                f"for( {stmt.rangeDecl} : {stmt.rangeExpression} )",
                stmt.body,
                cx,
            )

    def _compile_while(self, stmt: WhileStatement, cx: Context) -> None:
        equivalent = ForStatement(condExpression=stmt.expression, body=stmt.body)
        equivalent.FirstSourceLine = stmt.FirstSourceLine
        self.CompileStatement(equivalent, cx)

    def _compile_loop(self, stmt: LoopStatement, cx: Context) -> None:
        equivalent = ForStatement(body=stmt.body)
        equivalent.FirstSourceLine = stmt.FirstSourceLine
        self.CompileStatement(equivalent, cx)

    def _compile_choose(self, stmt: ChooseStatement, cx: Context) -> None:
        group = self.chooseGroups + 1
        self.chooseGroups = group

        if not isinstance(stmt.body, CodeBlock):
            raise Error(
                stmt.FirstSourceLine,
                "'choose' must be followed by a compound statement.",
            )
        codeblock: CodeBlock = stmt.body
        choices = []
        for i, ch in enumerate(codeblock.statements):
            if not isinstance(ch, WhenStatement):
                raise Error(
                    ch.FirstSourceLine,
                    "only 'when' statements are valid in an 'choose' block.",
                )
            callbackType = "{3}< {0}, {1}, {2} >".format(
                self.fullClassName,
                self.whenCount + i,
                ch.wait.result.type,
                "ActorSingleCallback" if ch.wait.isWaitNext else "ActorCallback",
            )
            callbackTypeInState = "{3}< {0}, {1}, {2} >".format(
                self.className,
                self.whenCount + i,
                ch.wait.result.type,
                "ActorSingleCallback" if ch.wait.isWaitNext else "ActorCallback",
            )
            body_func = self.getFunction(
                cx.target.name,
                "when",
                [
                    f"{ch.wait.result.type} const& {'__' if ch.wait.resultIsState else ''}{ch.wait.result.name}",
                    ActorCompiler.loopDepth,
                ],
                [
                    f"{ch.wait.result.type} && {'__' if ch.wait.resultIsState else ''}{ch.wait.result.name}",
                    ActorCompiler.loopDepth,
                ],
            )
            choices.append(
                {
                    "Stmt": ch,
                    "Group": group,
                    "Index": self.whenCount + i,
                    "Body": body_func,
                    "Future": f"__when_expr_{self.whenCount + i}",
                    "CallbackType": callbackType,
                    "CallbackTypeInStateClass": callbackTypeInState,
                }
            )
        self.whenCount += len(choices)

        exitFunc = self.getFunction("exitChoose", "", [])
        exitFunc.returnType = "void"
        exitFunc.WriteLine(
            "if ({0}->actor_wait_state > 0) {0}->actor_wait_state = 0;", self.This
        )
        for ch in choices:
            exitFunc.WriteLine(
                "{0}->{1}::remove();", self.This, ch["CallbackTypeInStateClass"]
            )
        exitFunc.endIsUnreachable = True

        reachable = False
        for ch in choices:
            self.callbacks.append(
                CallbackVar(
                    SourceLine=ch["Stmt"].FirstSourceLine,
                    CallbackGroup=ch["Group"],
                    type=ch["CallbackType"],
                )
            )
            r = ch["Body"]
            if ch["Stmt"].wait.resultIsState:
                overload = r.popOverload()
                state_decl = StateDeclarationStatement(
                    decl=VarDeclaration(
                        type=ch["Stmt"].wait.result.type,
                        name=ch["Stmt"].wait.result.name,
                        initializer="__" + ch["Stmt"].wait.result.name,
                        initializerConstructorSyntax=False,
                    )
                )
                state_decl.FirstSourceLine = ch["Stmt"].FirstSourceLine
                self.CompileStatement(state_decl, cx.WithTarget(r))
                if overload is not None:
                    overload.WriteLine(
                        "{0} = std::move(__{0});", ch["Stmt"].wait.result.name
                    )
                    r.setOverload(overload)
            if ch["Stmt"].body is not None:
                r = self.Compile(
                    self.AsCodeBlock(ch["Stmt"].body), cx.WithTarget(r), True
                ).target
            if r is not None:
                reachable = True
                if len(cx.next.formalParameters) == 1:
                    r.WriteLine("loopDepth = {0};", cx.next.call("loopDepth"))
                else:
                    overload = r.popOverload()
                    r.WriteLine(
                        "loopDepth = {0};",
                        cx.next.call(ch["Stmt"].wait.result.name, "loopDepth"),
                    )
                    if overload is not None:
                        overload.WriteLine(
                            "loopDepth = {0};",
                            cx.next.call(
                                f"std::move({ch['Stmt'].wait.result.name})", "loopDepth"
                            ),
                        )
                        r.setOverload(overload)

            cbFunc = Function()
            cbFunc.name = "callback_fire"
            cbFunc.returnType = "void"
            cbFunc.formalParameters = [
                ch["CallbackTypeInStateClass"] + "*",
                ch["Stmt"].wait.result.type + " const& value",
            ]
            cbFunc.endIsUnreachable = True
            cbFunc.addOverload(
                ch["CallbackTypeInStateClass"] + "*",
                ch["Stmt"].wait.result.type + " && value",
            )
            self.functions[f"{cbFunc.name}#{ch['Index']}"] = cbFunc
            cbFunc.Indent(ActorCompiler.codeIndent)
            self.ProbeEnter(cbFunc, self.actor.name, ch["Index"])
            cbFunc.WriteLine("{0};", exitFunc.call())

            _overload = cbFunc.popOverload()
            self.TryCatch(
                cx.WithTarget(cbFunc),
                cx.catchFErr,
                cx.tryLoopDepth,
                lambda: cbFunc.WriteLine("{0};", ch["Body"].call("value", "0")),
                False,
            )
            if _overload is not None:
                self.TryCatch(
                    cx.WithTarget(_overload),
                    cx.catchFErr,
                    cx.tryLoopDepth,
                    lambda: _overload.WriteLine(
                        "{0};", ch["Body"].call("std::move(value)", "0")
                    ),
                    False,
                )
                cbFunc.setOverload(_overload)
            self.ProbeExit(cbFunc, self.actor.name, ch["Index"])

            errFunc = Function()
            errFunc.name = "callback_error"
            errFunc.returnType = "void"
            errFunc.formalParameters = [
                ch["CallbackTypeInStateClass"] + "*",
                "Error err",
            ]
            errFunc.endIsUnreachable = True
            self.functions[f"{errFunc.name}#{ch['Index']}"] = errFunc
            errFunc.Indent(ActorCompiler.codeIndent)
            self.ProbeEnter(errFunc, self.actor.name, ch["Index"])
            errFunc.WriteLine("{0};", exitFunc.call())
            self.TryCatch(
                cx.WithTarget(errFunc),
                cx.catchFErr,
                cx.tryLoopDepth,
                lambda: errFunc.WriteLine("{0};", cx.catchFErr.call("err", "0")),
                False,
            )
            self.ProbeExit(errFunc, self.actor.name, ch["Index"])

        firstChoice = True
        for ch in choices:
            getFunc = "pop" if ch["Stmt"].wait.isWaitNext else "get"
            self.LineNumberFunction(cx.target, ch["Stmt"].wait.FirstSourceLine)
            cx.target.WriteLine(
                "{2}<{3}> {0} = {1};",
                ch["Future"],
                ch["Stmt"].wait.futureExpression,
                "FutureStream" if ch["Stmt"].wait.isWaitNext else "StrictFuture",
                ch["Stmt"].wait.result.type,
            )
            if firstChoice:
                firstChoice = False
                self.LineNumberFunction(cx.target, stmt.FirstSourceLine)
                if self.actor.IsCancellable():
                    cx.target.WriteLine(
                        "if ({1}->actor_wait_state < 0) return {0};",
                        cx.catchFErr.call(
                            "actor_cancelled()", self.AdjustLoopDepth(cx.tryLoopDepth)
                        ),
                        self.This,
                    )
            cx.target.WriteLine(
                "if ({0}.isReady()) {{ if ({0}.isError()) return {2}; else return {1}; }};",
                ch["Future"],
                ch["Body"].call(f"{ch['Future']}.{getFunc}()", "loopDepth"),
                cx.catchFErr.call(
                    f"{ch['Future']}.getError()", self.AdjustLoopDepth(cx.tryLoopDepth)
                ),
            )
        cx.target.WriteLine("{1}->actor_wait_state = {0};", group, self.This)
        for ch in choices:
            self.LineNumberFunction(cx.target, ch["Stmt"].wait.FirstSourceLine)
            cx.target.WriteLine(
                "{0}.addCallbackAndClear(static_cast<{1}*>({2}));",
                ch["Future"],
                ch["CallbackTypeInStateClass"],
                self.This,
            )
        cx.target.WriteLine("loopDepth = 0;")
        if not reachable:
            cx.unreachable()

    def _compile_break(self, stmt: BreakStatement, cx: Context) -> None:
        if cx.breakF is None:
            raise Error(stmt.FirstSourceLine, "break outside loop")
        if isinstance(cx.breakF, LiteralBreak):
            cx.target.WriteLine("{0};", cx.breakF.call())
        else:
            cx.target.WriteLine(
                "return {0}; // break", cx.breakF.call("loopDepth==0?0:loopDepth-1")
            )
        cx.unreachable()

    def _compile_continue(self, stmt: ContinueStatement, cx: Context) -> None:
        if cx.continueF is None:
            raise Error(stmt.FirstSourceLine, "continue outside loop")
        if isinstance(cx.continueF, LiteralContinue):
            cx.target.WriteLine("{0};", cx.continueF.call())
        else:
            cx.target.WriteLine(
                "return {0}; // continue", cx.continueF.call("loopDepth")
            )
        cx.unreachable()

    def _compile_wait(self, stmt: WaitStatement, cx: Context) -> None:
        equiv = ChooseStatement(
            body=CodeBlock(
                statements=[
                    WhenStatement(wait=stmt, body=None),
                ]
            )
        )
        equiv.FirstSourceLine = stmt.FirstSourceLine
        equiv.body.FirstSourceLine = stmt.FirstSourceLine
        equiv.body.statements[0].FirstSourceLine = stmt.FirstSourceLine
        if not stmt.resultIsState:
            cx.next.formalParameters = [
                f"{stmt.result.type} const& {stmt.result.name}",
                ActorCompiler.loopDepth,
            ]
            cx.next.addOverload(
                f"{stmt.result.type} && {stmt.result.name}", ActorCompiler.loopDepth
            )
        self.CompileStatement(equiv, cx)

    def _compile_codeblock(self, stmt: CodeBlock, cx: Context) -> None:
        cx.target.WriteLine("{")
        cx.target.Indent(+1)
        end = self.Compile(stmt, cx, True)
        cx.target.Indent(-1)
        cx.target.WriteLine("}")
        if end.target is None:
            cx.unreachable()
        elif end.target != cx.target:
            end.target.WriteLine("loopDepth = {0};", cx.next.call("loopDepth"))

    def _compile_return(self, stmt: ReturnStatement, cx: Context) -> None:
        self.LineNumberFunction(cx.target, stmt.FirstSourceLine)
        if (stmt.expression == "") != (self.actor.returnType is None):
            raise Error(
                stmt.FirstSourceLine,
                "Return statement does not match actor declaration",
            )
        if self.actor.returnType is not None:
            if stmt.expression == "Never()":
                cx.target.WriteLine("this->~{0}();", self.stateClassName)
                cx.target.WriteLine("{0}->sendAndDelPromiseRef(Never());", self.This)
            else:
                cx.target.WriteLine(
                    "if (!{0}->SAV<{1}>::futures) {{ (void)({2}); this->~{3}(); {0}->destroy(); return 0; }}",
                    self.This,
                    self.actor.returnType,
                    stmt.expression,
                    self.stateClassName,
                )
                if any(s.name == stmt.expression for s in self.state):
                    cx.target.WriteLine(
                        "new (&{0}->SAV< {1} >::value()) {1}(std::move({2})); // state_var_RVO",
                        self.This,
                        self.actor.returnType,
                        stmt.expression,
                    )
                else:
                    cx.target.WriteLine(
                        "new (&{0}->SAV< {1} >::value()) {1}({2});",
                        self.This,
                        self.actor.returnType,
                        stmt.expression,
                    )
                cx.target.WriteLine("this->~{0}();", self.stateClassName)
                cx.target.WriteLine("{0}->finishSendAndDelPromiseRef();", self.This)
        else:
            cx.target.WriteLine("delete {0};", self.This)
        cx.target.WriteLine("return 0;")
        cx.unreachable()

    def _compile_if(self, stmt: IfStatement, cx: Context) -> None:
        useContinuation = self.WillContinue(stmt.ifBody) or self.WillContinue(
            stmt.elseBody
        )
        self.LineNumberFunction(cx.target, stmt.FirstSourceLine)
        cx.target.WriteLine(
            "if {1}({0})", stmt.expression, "constexpr " if stmt.constexpr else ""
        )
        cx.target.WriteLine("{")
        cx.target.Indent(+1)
        ifTarget = self.Compile(
            self.AsCodeBlock(stmt.ifBody), cx, useContinuation
        ).target
        if useContinuation and ifTarget is not None:
            ifTarget.WriteLine("loopDepth = {0};", cx.next.call("loopDepth"))
        cx.target.Indent(-1)
        cx.target.WriteLine("}")
        elseTarget = None
        if stmt.elseBody is not None or useContinuation:
            cx.target.WriteLine("else")
            cx.target.WriteLine("{")
            cx.target.Indent(+1)
            elseTarget = cx.target
            if stmt.elseBody is not None:
                elseTarget = self.Compile(
                    self.AsCodeBlock(stmt.elseBody), cx, useContinuation
                ).target
            if useContinuation and elseTarget is not None:
                elseTarget.WriteLine("loopDepth = {0};", cx.next.call("loopDepth"))
            cx.target.Indent(-1)
            cx.target.WriteLine("}")
        if ifTarget is None and stmt.elseBody is not None and elseTarget is None:
            cx.unreachable()
        elif not cx.next.wasCalled and useContinuation:
            raise Exception("Internal error: IfStatement: next not called?")

    def _compile_try(self, stmt: TryStatement, cx: Context) -> None:
        reachable = False
        if len(stmt.catches) != 1:
            raise Error(
                stmt.FirstSourceLine, "try statement must have exactly one catch clause"
            )
        c = stmt.catches[0]
        catchErrorParameterName = ""
        if c.expression != "...":
            exp = c.expression.replace(" ", "")
            if not exp.startswith("Error&"):
                raise Error(
                    c.FirstSourceLine,
                    "Only type 'Error' or '...' may be caught in an actor function",
                )
            catchErrorParameterName = exp[6:]
        if catchErrorParameterName == "":
            catchErrorParameterName = "__current_error"
        catchFErr = self.getFunction(
            cx.target.name,
            "Catch",
            [f"const Error& {catchErrorParameterName}", ActorCompiler.loopDepth0],
        )
        catchFErr.exceptionParameterIs = catchErrorParameterName
        end = self.TryCatchCompile(
            self.AsCodeBlock(stmt.tryBody), cx.WithCatch(catchFErr)
        )
        if end.target is not None:
            reachable = True
            self.TryCatch(
                end,
                cx.catchFErr,
                cx.tryLoopDepth,
                lambda: end.target.WriteLine(
                    "loopDepth = {0};", cx.next.call("loopDepth")
                ),
            )

        def catch_action() -> None:
            cend = self.Compile(
                self.AsCodeBlock(c.body), cx.WithTarget(catchFErr), True
            )
            if cend.target is not None:
                cend.target.WriteLine("loopDepth = {0};", cx.next.call("loopDepth"))
            nonlocal_reachable[0] = nonlocal_reachable[0] or cend.target is not None

        nonlocal_reachable = [reachable]
        self.TryCatch(
            cx.WithTarget(catchFErr), cx.catchFErr, cx.tryLoopDepth, catch_action
        )
        if not nonlocal_reachable[0]:
            cx.unreachable()

    def _compile_throw(self, stmt: ThrowStatement, cx: Context) -> None:
        self.LineNumberFunction(cx.target, stmt.FirstSourceLine)
        if stmt.expression == "":
            if cx.target.exceptionParameterIs is not None:
                cx.target.WriteLine(
                    "return {0};",
                    cx.catchFErr.call(
                        cx.target.exceptionParameterIs,
                        self.AdjustLoopDepth(cx.tryLoopDepth),
                    ),
                )
            else:
                raise Error(
                    stmt.FirstSourceLine,
                    "throw statement with no expression has no current exception in scope",
                )
        else:
            cx.target.WriteLine(
                "return {0};",
                cx.catchFErr.call(
                    stmt.expression, self.AdjustLoopDepth(cx.tryLoopDepth)
                ),
            )
        cx.unreachable()

    def getIteratorName(self, cx: Context) -> str:
        name = "RangeFor" + cx.target.name + "Iterator"
        if name not in self.iterators:
            self.iterators[name] = 0
        result = f"{name}{self.iterators[name]}"
        self.iterators[name] += 1
        return result

    def getFunction(
        self,
        baseName: str,
        addName: str,
        formalParameters: Sequence[str],
        overloadFormalParameters: Optional[Sequence[str]] = None,
    ) -> Function:
        if addName == "cont" and len(baseName) >= 5 and baseName[-5:-1] == "cont":
            proposedName = baseName[:-1]
        else:
            proposedName = baseName + addName
        i = 0
        while f"{proposedName}{i + 1}" in self.functions:
            i += 1
        i += 1
        f = Function()
        f.name = f"{proposedName}{i}"
        f.returnType = "int"
        if isinstance(formalParameters, str):
            f.formalParameters = [formalParameters]
        else:
            f.formalParameters = list(formalParameters)
        if overloadFormalParameters is not None:
            if isinstance(overloadFormalParameters, str):
                f.addOverload(overloadFormalParameters)
            else:
                f.addOverload(*overloadFormalParameters)
        f.Indent(ActorCompiler.codeIndent)
        self.functions[f.name] = f
        return f

    def WriteFunction(self, writer: io.TextIOBase, func: Function, body: str) -> None:
        writer.write(
            f"{ActorCompiler.memberIndentStr}{'' if func.returnType == '' else func.returnType + ' '}{func.useByName()}({','.join(func.formalParameters)}){'' if func.specifiers == '' else ' ' + func.specifiers}\n"
        )
        if func.returnType != "":
            writer.write(f"{ActorCompiler.memberIndentStr}" + "{\n")
        writer.write(body)
        writer.write("\n")
        if not func.endIsUnreachable:
            writer.write(f"{ActorCompiler.memberIndentStr}\treturn loopDepth;\n")
        writer.write(f"{ActorCompiler.memberIndentStr}" + "}\n")

    def WriteFunctions(self, writer: io.TextIOBase) -> None:
        for func in self.functions.values():
            body = func.BodyText
            if body:
                self.WriteFunction(writer, func, body)
            if func.overload is not None:
                overloadBody = func.overload.BodyText
                if overloadBody:
                    self.WriteFunction(writer, func.overload, overloadBody)

    def WriteCancelFunc(self, writer: io.TextIOBase) -> None:
        if self.actor.IsCancellable():
            cancelFunc = Function()
            cancelFunc.name = "cancel"
            cancelFunc.returnType = "void"
            cancelFunc.formalParameters = []
            cancelFunc.endIsUnreachable = True
            cancelFunc.publicName = True
            cancelFunc.specifiers = "override"
            cancelFunc.Indent(ActorCompiler.codeIndent)
            cancelFunc.WriteLine("auto wait_state = this->actor_wait_state;")
            cancelFunc.WriteLine("this->actor_wait_state = -1;")
            cancelFunc.WriteLine("switch (wait_state) {")
            lastGroup = -1
            for cb in sorted(self.callbacks, key=lambda c: c.CallbackGroup):
                if cb.CallbackGroup != lastGroup:
                    lastGroup = cb.CallbackGroup
                    cancelFunc.WriteLine(
                        "case {0}: this->a_callback_error(({1}*)0, actor_cancelled()); break;",
                        cb.CallbackGroup,
                        cb.type,
                    )
            cancelFunc.WriteLine("}")
            self.WriteFunction(writer, cancelFunc, cancelFunc.BodyText)

    def WriteConstructor(
        self, body: Function, writer: io.TextIOBase, fullStateClassName: str
    ) -> None:
        constructor = Function()
        constructor.name = self.className
        constructor.returnType = ""
        constructor.formalParameters = self.ParameterList()
        constructor.endIsUnreachable = True
        constructor.publicName = True
        constructor.Indent(ActorCompiler.codeIndent)
        constructor.WriteLine(
            " : Actor<{0}>(),",
            "void" if self.actor.returnType is None else self.actor.returnType,
        )
        constructor.WriteLine(
            "   {0}({1})",
            fullStateClassName,
            ", ".join(p.name for p in self.actor.parameters),
        )
        constructor.Indent(-1)
        constructor.WriteLine("{")
        constructor.Indent(+1)
        self.ProbeEnter(constructor, self.actor.name)
        constructor.WriteLine("#ifdef ENABLE_SAMPLING")
        constructor.WriteLine('this->lineage.setActorName("{0}");', self.actor.name)
        constructor.WriteLine("LineageScope _(&this->lineage);")
        constructor.WriteLine("#endif")
        constructor.WriteLine("this->{0};", body.call())
        self.ProbeExit(constructor, self.actor.name)
        self.WriteFunction(writer, constructor, constructor.BodyText)

    def WriteStateConstructor(self, writer: io.TextIOBase) -> None:
        constructor = Function()
        constructor.name = self.stateClassName
        constructor.returnType = ""
        constructor.formalParameters = self.ParameterList()
        constructor.endIsUnreachable = True
        constructor.publicName = True
        constructor.Indent(ActorCompiler.codeIndent)
        ini = None
        line = self.actor.SourceLine
        for s in list(self.state):
            if s.initializer is not None:
                self.LineNumberFunction(constructor, line)
                if ini is not None:
                    constructor.WriteLine(ini + ",")
                    ini = "   "
                else:
                    ini = " : "
                ini += f"{s.name}({s.initializer})"
                line = s.SourceLine
        self.LineNumberFunction(constructor, line)
        if ini is not None:
            constructor.WriteLine(ini)
        constructor.Indent(-1)
        constructor.WriteLine("{")
        constructor.Indent(+1)
        self.ProbeCreate(constructor, self.actor.name)
        self.WriteFunction(writer, constructor, constructor.BodyText)

    def WriteStateDestructor(self, writer: io.TextIOBase) -> None:
        destructor = Function()
        destructor.name = f"~{self.stateClassName}"
        destructor.returnType = ""
        destructor.formalParameters = []
        destructor.endIsUnreachable = True
        destructor.publicName = True
        destructor.Indent(ActorCompiler.codeIndent)
        destructor.Indent(-1)
        destructor.WriteLine("{")
        destructor.Indent(+1)
        self.ProbeDestroy(destructor, self.actor.name)
        self.WriteFunction(writer, destructor, destructor.BodyText)

    def ParameterList(self) -> List[str]:
        params = []
        for p in self.actor.parameters:
            if p.initializer != "":
                params.append(f"{p.type} const& {p.name} = {p.initializer}")
            else:
                params.append(f"{p.type} const& {p.name}")
        return params

    def AdjustLoopDepth(self, subtract: int) -> str:
        if subtract == 0:
            return "loopDepth"
        return f"std::max(0, loopDepth - {subtract})"

    def Flatten(self, stmt: Optional[Statement]) -> Iterable[Statement]:
        if stmt is None:
            return []
        fl = (
            TypeSwitch(stmt)
            .Case(LoopStatement, lambda s: self.Flatten(s.body))
            .Case(WhileStatement, lambda s: self.Flatten(s.body))
            .Case(ForStatement, lambda s: self.Flatten(s.body))
            .Case(RangeForStatement, lambda s: self.Flatten(s.body))
            .Case(
                CodeBlock, lambda s: (t for x in s.statements for t in self.Flatten(x))
            )
            .Case(
                IfStatement,
                lambda s: chain(self.Flatten(s.ifBody), self.Flatten(s.elseBody)),
            )
            .Case(ChooseStatement, lambda s: self.Flatten(s.body))
            .Case(WhenStatement, lambda s: self.Flatten(s.body))
            .Case(
                TryStatement,
                lambda s: chain(
                    self.Flatten(s.tryBody), *(self.Flatten(c.body) for c in s.catches)
                ),
            )
            .Case(Statement, lambda s: [])
            .Return()
        )
        return chain([stmt], fl)

    def FindState(self) -> None:
        self.state = [
            StateVar(
                SourceLine=self.actor.SourceLine,
                name=p.name,
                type=p.type,
                initializer=p.name,
                initializerConstructorSyntax=False,
            )
            for p in self.actor.parameters
        ]

    def Compile(
        self, block: CodeBlock, context: Context, okToContinue: bool = True
    ) -> Context:
        cx = context.Clone()
        cx.next = None
        for stmt in block.statements:
            if cx.target is None:
                raise Error(stmt.FirstSourceLine, "Unreachable code.")
            if cx.next is None:
                cx.next = self.getFunction(
                    cx.target.name, "cont", ActorCompiler.loopDepth
                )
            self.CompileStatement(stmt, cx)
            if cx.next.wasCalled:
                if cx.target is None:
                    raise Exception("Unreachable continuation called?")
                if not okToContinue:
                    raise Exception("Unexpected continuation")
                cx.target = cx.next
                cx.next = None
        return cx


def compile_to_string(
    source_path: str,
    dest_path: str,
    disable_diagnostics: bool = False,
    generate_probes: bool = False,
) -> str:
    policy = ErrorMessagePolicy()
    policy.DisableDiagnostics = disable_diagnostics
    source_norm = os.path.abspath(source_path)
    with open(source_norm, "r", encoding="utf-8") as f:
        input_data = f.read()
    # Reset per-invocation global naming state to mirror C# executable behavior (one file per process)
    ActorCompiler.usedClassNames = set()
    parser = ActorParser(
        input_data, source_norm.replace("\\", "/"), policy, generate_probes
    )
    output_stream = io.StringIO()
    parser.Write(output_stream, dest_path.replace("\\", "/"))
    return output_stream.getvalue()


def compile_file(
    input_path: str, output_path: str, disable_diagnostics: bool, generate_probes: bool
) -> int:
    tmp_path = output_path + ".tmp"
    policy = ErrorMessagePolicy()
    policy.DisableDiagnostics = disable_diagnostics
    source_abs = os.path.abspath(input_path)
    # Reset per-invocation global naming state to mirror C# executable behavior (one file per process)
    ActorCompiler.usedClassNames = set()
    try:
        with open(source_abs, "r", encoding="utf-8") as f:
            input_data = f.read()
        with open(tmp_path, "w", encoding="utf-8", newline="") as output_stream:
            ActorParser(
                input_data, source_abs.replace("\\", "/"), policy, generate_probes
            ).Write(output_stream, output_path.replace("\\", "/"))
        if os.path.exists(output_path):
            os.remove(output_path)
        os.replace(tmp_path, output_path)
        return 0
    except Error as e:
        sys.stderr.write(f"{source_abs}({e.SourceLine}): error FAC1000: {e}\n")
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        if os.path.exists(output_path):
            os.remove(output_path)
        return 1
    except Exception as e:
        sys.stderr.write(f"{source_abs}(1): error FAC2000: Internal {e}\n")
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        if os.path.exists(output_path):
            os.remove(output_path)
        return 3


def verify_pregenerated(
    pregen_root: str, src_root: str, disable_diagnostics: bool, generate_probes: bool
) -> int:
    mismatches: List[str] = []
    for dirpath, _, filenames in os.walk(pregen_root):
        for name in filenames:
            if not (name.endswith(".actor.g.cpp") or name.endswith(".actor.g.h")):
                continue
            pregen_path = os.path.join(dirpath, name)
            rel = os.path.relpath(pregen_path, pregen_root)
            src_rel = rel.replace(".actor.g.", ".actor.")
            source_path = os.path.join(src_root, src_rel)
            if not os.path.exists(source_path):
                mismatches.append(f"Missing source for {pregen_path}")
                continue
            dest_override = None
            with open(pregen_path, "r", encoding="utf-8") as preg:
                for line in preg:
                    if ".actor.g." in line and "#line" in line:
                        m = re.search(r'#line \\d+ "([^"]+)"', line)
                        if m:
                            dest_override = m.group(1)
                            break
            generated = compile_to_string(
                source_path,
                dest_override if dest_override else pregen_path,
                disable_diagnostics,
                generate_probes,
            )
            with open(pregen_path, "r", encoding="utf-8") as f:
                pregen_content = f.read()
            if generated != pregen_content:
                mismatches.append(pregen_path)
    if mismatches:
        sys.stderr.write("Verification failed for the following files:\n")
        for m in mismatches:
            sys.stderr.write(f"  {m}\n")
        return 1
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Flow actor compiler (Python)")
    parser.add_argument("input", nargs="?", help="Input .actor.[cpp|h] file")
    parser.add_argument("output", nargs="?", help="Output .actor.g.[cpp|h] file")
    parser.add_argument(
        "--disable-diagnostics", action="store_true", help="Disable diagnostics"
    )
    parser.add_argument(
        "--generate-probes", action="store_true", help="Generate probe hooks"
    )
    parser.add_argument(
        "--verify-pregenerated",
        action="store_true",
        help="Regenerate all actor outputs and compare with pregenerated/",
    )
    parser.add_argument(
        "--pregenerated-root",
        default="pregenerated",
        help="Path to pregenerated directory",
    )
    parser.add_argument(
        "--src-root", default=".", help="Source root containing original actor files"
    )
    args = parser.parse_args(argv)

    if args.verify_pregenerated:
        return verify_pregenerated(
            os.path.abspath(args.pregenerated_root),
            os.path.abspath(args.src_root),
            args.disable_diagnostics,
            args.generate_probes,
        )

    if not args.input or not args.output:
        parser.error(
            "input and output are required unless --verify-pregenerated is used"
        )
    return compile_file(
        args.input, args.output, args.disable_diagnostics, args.generate_probes
    )


if __name__ == "__main__":
    sys.exit(main())
