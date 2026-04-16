#!/usr/bin/env python3
"""Python reimplementation of the vexillographer option generator."""

import os
import sys
import textwrap
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from enum import Enum
from typing import List, Optional, Sequence


class Scope(Enum):
    NetworkOption = "NetworkOption"
    DatabaseOption = "DatabaseOption"
    TransactionOption = "TransactionOption"
    StreamingMode = "StreamingMode"
    MutationType = "MutationType"
    ConflictRangeType = "ConflictRangeType"
    ErrorPredicate = "ErrorPredicate"


SCOPE_ORDER: Sequence[Scope] = [
    Scope.NetworkOption,
    Scope.DatabaseOption,
    Scope.TransactionOption,
    Scope.StreamingMode,
    Scope.MutationType,
    Scope.ConflictRangeType,
    Scope.ErrorPredicate,
]


class ParamType(Enum):
    NONE = "None"
    STRING = "String"
    INT = "Int"
    BYTES = "Bytes"


@dataclass
class Option:
    scope: Scope
    name: str
    code: int
    param_type: ParamType
    param_desc: Optional[str]
    comment: str
    hidden: bool
    persistent: bool
    sensitive: bool
    default_for: int

    @property
    def parameter_comment(self) -> str:
        if self.param_desc is None:
            return "Option takes no parameter"
        return f"({self.param_type.value}) {self.param_desc}"

    def is_deprecated(self) -> bool:
        return self.comment.startswith("Deprecated")


def usage() -> None:
    print(
        f"{sys.argv[0]} inputFile {{c,cpp,java,ruby,python}} <outputDirectory/outputFile>"
    )


def parse_bool(value: Optional[str]) -> bool:
    return value == "true"


def parse_param_type(value: Optional[str]) -> ParamType:
    if value is None:
        return ParamType.NONE
    if value == "String":
        return ParamType.STRING
    if value == "Int":
        return ParamType.INT
    if value == "Bytes":
        return ParamType.BYTES
    raise ValueError(f"Unsupported param type: {value}")


def scope_description(scope: Scope) -> str:
    if scope == Scope.NetworkOption:
        return "NET_OPTION"
    if scope == Scope.DatabaseOption:
        return "DB_OPTION"
    if scope == Scope.TransactionOption:
        return "TR_OPTION"
    if scope == Scope.StreamingMode:
        return "STREAMING_MODE"
    if scope == Scope.MutationType:
        return "MUTATION_TYPE"
    if scope == Scope.ConflictRangeType:
        return "CONFLICT_RANGE_TYPE"
    if scope == Scope.ErrorPredicate:
        return "ERROR_PREDICATE"
    raise ValueError(f"Unknown scope {scope}")


def parse_options(path: str, binding: str) -> List[Option]:
    root = ET.parse(path).getroot()
    options: List[Option] = []
    for scope_elem in root.findall("Scope"):
        scope_name = scope_elem.attrib["name"]
        try:
            scope = Scope(scope_name)
        except ValueError as exc:
            raise ValueError(f"Unknown scope {scope_name}") from exc
        for opt_elem in scope_elem.findall("Option"):
            disable_on = opt_elem.attrib.get("disableOn")
            if disable_on:
                disabled_bindings = disable_on.split(",")
                if binding in disabled_bindings:
                    continue

            param_type = parse_param_type(opt_elem.attrib.get("paramType"))
            default_for_attr = opt_elem.attrib.get("defaultFor")
            default_for = int(default_for_attr) if default_for_attr is not None else -1
            options.append(
                Option(
                    scope=scope,
                    name=opt_elem.attrib["name"],
                    code=int(opt_elem.attrib["code"]),
                    param_type=param_type,
                    param_desc=opt_elem.attrib.get("paramDescription"),
                    comment=opt_elem.attrib.get("description", "") or "",
                    hidden=parse_bool(opt_elem.attrib.get("hidden")),
                    persistent=parse_bool(opt_elem.attrib.get("persistent")),
                    sensitive=parse_bool(opt_elem.attrib.get("sensitive")),
                    default_for=default_for,
                )
            )
    return options


class BindingWriter:
    def write_files(self, output_file: List[str], options: Sequence[Option]) -> None:  # pragma: no cover - interface
        raise NotImplementedError


class CWriter(BindingWriter):
    @staticmethod
    def _get_c_line(option: Option, indent: str, prefix: str) -> str:
        parameter_comment = ""
        if option.scope.value.endswith("Option"):
            parameter_comment = (
                f"{indent}/* Parameter: {option.parameter_comment} "
                f"{'This is a hidden parameter and should not be used directly by applications.' if option.hidden else ''}*/\n"
            )
        return (
            f"{indent}/* {option.comment} */\n"
            f"{parameter_comment}"
            f"{indent}{prefix}{option.name.upper()}={option.code}"
        )

    @staticmethod
    def _write_enum(out_file, scope: Scope, options: Sequence[Option]) -> None:
        out_file.write("typedef enum {\n")
        prefix = f"FDB_{scope_description(scope)}_"
        if not options:
            options = [
                Option(
                    scope=scope,
                    name="DUMMY_DO_NOT_USE",
                    code=-1,
                    param_type=ParamType.NONE,
                    param_desc=None,
                    comment="This option is only a placeholder for C compatibility and should not be used",
                    hidden=False,
                    persistent=False,
                    sensitive=False,
                    default_for=-1,
                )
            ]
        lines = [CWriter._get_c_line(opt, "    ", prefix) for opt in options]
        out_file.write(",\n\n".join(lines))
        out_file.write(f"\n}} FDB{scope.value};\n\n")

    def write_files(self, output_file: List[str], options: Sequence[Option]) -> None:
        os.makedirs(os.path.dirname(output_file[0]) or ".", exist_ok=True)
        with open(output_file[0], "w", newline="\n") as out_file:
            out_file.write(
                textwrap.dedent(
                    """\
                    #ifndef FDB_C_OPTIONS_G_H
                    #define FDB_C_OPTIONS_G_H
                    #pragma once

                    /*
                     * FoundationDB C API
                     *
                     * This source file is part of the FoundationDB open source project
                     *
                     * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
                     *
                     * Licensed under the Apache License, Version 2.0 (the 'License');
                     * you may not use this file except in compliance with the License.
                     * You may obtain a copy of the License at
                     *
                     *     http://www.apache.org/licenses/LICENSE-2.0
                     *
                     * Unless required by applicable law or agreed to in writing, software
                     * distributed under the License is distributed on an 'AS IS' BASIS,
                     * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
                     * See the License for the specific language governing permissions and
                     * limitations under the License.
                     * Do not include this file directly.
                     */

                    """
                )
            )
            for scope in SCOPE_ORDER:
                scope_options = [opt for opt in options if opt.scope == scope]
                self._write_enum(out_file, scope, scope_options)
            out_file.write("#endif\n")


class CppWriter(BindingWriter):
    @staticmethod
    def _cpp_enum_line(option: Option, indent: str, prefix: str) -> str:
        return CWriter._get_c_line(option, indent, prefix)

    @staticmethod
    def _write_enum(out_file, scope: Scope, options: Sequence[Option]) -> None:
        out_file.write(f"struct FDB{scope.value}s {{\n")
        out_file.write(f"\tfriend class FDBOptionInfoMap<FDB{scope.value}s>;\n\n")
        out_file.write("\tenum Option : int {\n")
        prefix = ""
        lines = [CppWriter._cpp_enum_line(opt, "\t\t", prefix) for opt in options]
        out_file.write(",\n\n".join(lines))
        out_file.write("\n\t};\n\n")
        out_file.write(f"\tstatic FDBOptionInfoMap<FDB{scope.value}s> optionInfo;\n\n")
        out_file.write("private:\n")
        out_file.write("\tstatic void init();\n")
        out_file.write("};\n\n")

    @staticmethod
    def _info_line(option: Option, indent: str, struct_name: str) -> str:
        return (
            f"{indent}ADD_OPTION_INFO({struct_name}, {option.name.upper()}, "
            f'"{option.name.upper()}", "{option.comment}", "{option.parameter_comment}", '
            f"{str(option.param_desc is not None).lower()}, "
            f"{str(option.hidden).lower()}, "
            f"{str(option.persistent).lower()}, "
            f"{str(option.sensitive).lower()}, "
            f"{option.default_for}, FDBOptionInfo::ParamType::{option.param_type.value})"
        )

    @staticmethod
    def _write_info(out_file, scope: Scope, options: Sequence[Option]) -> None:
        out_file.write(
            f"FDBOptionInfoMap<FDB{scope.value}s> FDB{scope.value}s::optionInfo;\n\n"
        )
        out_file.write(f"void FDB{scope.value}s::init() {{\n")
        lines = [
            CppWriter._info_line(opt, "\t", f"FDB{scope.value}s") for opt in options
        ]
        out_file.write("\n".join(lines))
        out_file.write("\n}\n\n")

    def write_files(self, output_files: List[str], options: Sequence[Option]) -> None:
        if output_files[0].endswith(".h"):
            header_path = output_files[0]
            cpp_path = output_files[1]
        else:
            header_path = output_files[1]
            cpp_path = output_files[0]
        os.makedirs(os.path.dirname(header_path) or ".", exist_ok=True)
        with open(header_path, "w", newline="\n") as header:
            header.write("#ifndef FDBCLIENT_FDBOPTIONS_G_H\n")
            header.write("#define FDBCLIENT_FDBOPTIONS_G_H\n")
            header.write("#pragma once\n\n")
            header.write('#include "fdbclient/FDBOptions.h"\n\n')
            for scope in SCOPE_ORDER:
                scope_options = [opt for opt in options if opt.scope == scope]
                self._write_enum(header, scope, scope_options)
            header.write("\n#endif\n")

        with open(cpp_path, "w", newline="\n") as cpp_file:
            cpp_file.write('#include "fdbclient/FDBOptions.g.h"\n\n')
            for scope in SCOPE_ORDER:
                scope_options = [opt for opt in options if opt.scope == scope]
                self._write_info(cpp_file, scope, scope_options)


class JavaWriter(BindingWriter):
    class ScopeOptions:
        def __init__(self, is_settable: bool, comment: str, is_public: bool = True):
            self.is_public = is_public
            self.is_settable_option = is_settable
            self.comment = comment

    scope_doc_options = {
        Scope.NetworkOption: ScopeOptions(
            True,
            "A set of options that can be set globally for the {@link FDB FoundationDB API}.",
        ),
        Scope.DatabaseOption: ScopeOptions(
            True, "A set of options that can be set on a {@link Database}."
        ),
        Scope.TransactionOption: ScopeOptions(
            True, "A set of options that can be set on a {@link Transaction}."
        ),
        Scope.StreamingMode: ScopeOptions(
            False,
            "Options that control the way the Java binding performs range reads. These options can be passed to {@link Transaction#getRange(byte[], byte[], int, boolean, StreamingMode) Transaction.getRange(...)}.",
        ),
        Scope.MutationType: ScopeOptions(
            False,
            "A set of operations that can be performed atomically on a database. These are used as parameters to {@link Transaction#mutate(MutationType, byte[], byte[])}.",
        ),
        Scope.ConflictRangeType: ScopeOptions(
            False, "Conflict range types used internally by the C API.", False
        ),
        Scope.ErrorPredicate: ScopeOptions(
            True,
            "Error code predicates for binding writers and non-standard layer implementers.",
        ),
    }

    @staticmethod
    def _format_comment(indent_tabs: int, comment_text: str) -> str:
        tabs = "\t" * indent_tabs
        lines = "".join(
            "\n" + tabs + " * " + line.strip() for line in comment_text.split("\n")
        )
        return f"{tabs}/**{lines}\n{tabs} */"

    @staticmethod
    def _replace_ticks(text: str) -> str:
        output = ""
        idx = 0
        while True:
            start = text.find("``", idx)
            if start < 0:
                output += text[idx:]
                break
            output += text[idx:start]
            end = text.find("``", start + 2)
            if end < 0:
                raise ValueError("No closing double tick")
            output += "{@code " + text[start + 2 : end] + "}"
            idx = end + 2
        return output

    @staticmethod
    def _to_camel_case(option_name: str) -> str:
        return "".join(part[:1].upper() + part[1:] for part in option_name.split("_"))

    @staticmethod
    def _set_func_name(option_name: str) -> str:
        return "set" + JavaWriter._to_camel_case(option_name)

    @staticmethod
    def _predicate_func_name(option_name: str) -> str:
        return "is" + JavaWriter._to_camel_case(option_name)

    @staticmethod
    def _java_type_name(param_type: ParamType) -> str:
        if param_type == ParamType.INT:
            return "long"
        if param_type == ParamType.BYTES:
            return "byte[]"
        if param_type == ParamType.STRING:
            return "String"
        raise ValueError("Unsupported operation type")

    @staticmethod
    def _get_enum(option: Option) -> str:
        main_comment = option.comment or "Currently undocumented."
        if not main_comment.endswith("."):
            main_comment += "."
        main_comment = JavaWriter._replace_ticks(main_comment)
        deprecated = "\t@Deprecated\n" if option.is_deprecated() else ""
        return (
            JavaWriter._format_comment(1, main_comment)
            + "\n"
            + deprecated
            + f"\t{option.name.upper()}({option.code})"
        )

    @staticmethod
    def _write_options_class(out_file, scope: Scope, options: Sequence[Option]) -> None:
        class_name = scope.value + "s"
        out_file.write("package com.apple.foundationdb;\n\n")
        comment = JavaWriter.scope_doc_options[scope].comment
        if not options:
            comment += "\n\nThere are currently no options available."
        out_file.write(JavaWriter._format_comment(0, comment) + "\n")
        out_file.write(f"public class {class_name} extends OptionsSet {{\n")
        out_file.write(f"\tpublic {class_name}( OptionConsumer consumer ) {{ super(consumer); }}\n")
        ordered = sorted(options, key=lambda o: o.comment == "")
        for opt in ordered:
            if opt.hidden:
                continue
            out_file.write("\n")
            if opt.comment:
                comment_text = opt.comment
                if not comment_text.endswith("."):
                    comment_text += "."
                if opt.param_desc is not None:
                    comment_text += "\n\n@param value " + opt.param_desc
                out_file.write(
                    JavaWriter._format_comment(
                        1, JavaWriter._replace_ticks(comment_text)
                    )
                    + "\n"
                )
            if opt.is_deprecated():
                out_file.write("\t@Deprecated\n")
            if opt.param_type == ParamType.NONE:
                out_file.write(
                    f"\tpublic void {JavaWriter._set_func_name(opt.name)}() {{ setOption({opt.code}); }}\n"
                )
            else:
                out_file.write(
                    f"\tpublic void {JavaWriter._set_func_name(opt.name)}({JavaWriter._java_type_name(opt.param_type)} value) {{ setOption({opt.code}, value); }}\n"
                )
        out_file.write("}")

    @staticmethod
    def _write_predicate_class(
        out_file, scope: Scope, options: Sequence[Option]
    ) -> None:
        out_file.write(
            textwrap.dedent(
                """\
                package com.apple.foundationdb;

                import com.apple.foundationdb.async.CloneableException;

                /**
                 * An Error from the native layers of FoundationDB.  Each {@code FDBException} sets
                 *  the {@code message} of the underlying Java {@link Exception}. FDB exceptions expose
                 *  a number of functions including, for example, {@link #isRetryable()} that
                 *  evaluate predicates on the internal FDB error. Most clients should use those methods
                 *  in order to implement special handling for certain errors if their application
                 *  requires it.
                 *
                 * <p>
                 * Errors in FDB should generally be retried if they match the {@link #isRetryable()}
                 *  predicate. In addition, as with any distributed system, certain classes of errors
                 *  may fail in such a way that it is unclear whether the transaction succeeded (they
                 *  {@link #isMaybeCommitted() may be committed} or not). To handle these cases, clients
                 *  are generally advised to make their database operations idempotent and to place
                 *  their operations within retry loops. The FDB Java API provides some default retry loops
                 *  within the {@link Database} interface. See the discussion within the documentation of
                 *  {@link Database#runAsync(Function) Database.runAsync()} for more details.
                 *
                 * @see com.apple.foundationdb.Transaction#onError(Throwable) Transaction.onError()
                 * @see com.apple.foundationdb.Database#runAsync(Function) Database.runAsync()
                 */
                public class FDBException extends RuntimeException implements CloneableException {
                    private static final long serialVersionUID = 1L;
                    private final int code;

                    /**
                     * A general constructor.  Not for use by client code.
                     *
                     * @param message error message of this exception
                     * @param code internal FDB error code of this exception
                     */
                    public FDBException(String message, int code) {
                        super(message);
                        this.code = code;
                    }

                    /**
                     * Gets the code for this error. A list of common errors codes
                     *  are published <a href="/foundationdb/api-error-codes.html">elsewhere within
                     *  our documentation</a>.
                     *
                     * @return the internal FDB error code
                     */
                    public int getCode() {
                        return code;
                    }

                    /**
                     * Determine if this {@code FDBException} represents a success code from the native layer.
                     *
                     * @return {@code true} if this error represents success, {@code false} otherwise
                     */
                    public boolean isSuccess() {
                        return getCode() == 0;
                    }

                    @Override
                    public Exception retargetClone() {
                        FDBException exception = new FDBException(getMessage(), code);
                        exception.initCause(this);
                        return exception;
                    }
                """
            )
        )

        ordered = sorted(options, key=lambda o: o.comment == "")
        for opt in ordered:
            if opt.hidden:
                continue
            out_file.write("\n")
            if opt.comment:
                comment = opt.comment
                if not comment.endswith("."):
                    comment += "."
                if opt.param_desc is not None:
                    comment += "\n\n@param value " + opt.param_desc
                comment += f"\n\n@return {{@code true}} if this {{@code FDBException}} is {{@code {opt.name}}}"
                out_file.write(
                    JavaWriter._format_comment(
                        1, JavaWriter._replace_ticks(comment)
                    )
                    + "\n"
                )
            if opt.is_deprecated():
                out_file.write("\t@Deprecated\n")
            out_file.write(
                f"\tpublic boolean {JavaWriter._predicate_func_name(opt.name)}() {{ return FDB.evalErrorPredicate({opt.code}, this.code); }}\n"
            )
        out_file.write("}")

    @staticmethod
    def _write_enum_class(out_file, scope: Scope, options: Sequence[Option]) -> None:
        scope_name = scope.value
        out_file.write("package com.apple.foundationdb;\n\n")
        comment = JavaWriter.scope_doc_options[scope].comment
        if not options:
            comment += "\n\nThere are currently no options available."
        out_file.write(JavaWriter._format_comment(0, comment) + "\n")
        visibility = (
            "public " if JavaWriter.scope_doc_options[scope].is_public else ""
        )
        visible_options = [opt for opt in options if not opt.hidden]
        out_file.write(f"{visibility}enum {scope_name} {{\n")
        enum_lines = [JavaWriter._get_enum(opt) for opt in visible_options]
        out_file.write(",\n\n".join(enum_lines) + ";\n")
        out_file.write(
            textwrap.dedent(
                f"""
                    private final int code;

                    {scope_name}(int code) {{
                        this.code = code;
                    }}

                    /**
                     * Gets the FoundationDB native-level constant code for a {{@code {scope_name}}}.
                     *
                     * @return the native code for a FoundationDB {{@code {scope_name}}} constant.
                     */
                    public int code() {{
                        return this.code;
                    }}
                }}
                """
            )
        )

    def write_files(self, output_files: List[str], options: Sequence[Option]) -> None:
        if len(output_files) == 0:
            raise ValueError("No output files provided for Java binding generation")
        output_directory = os.path.dirname(output_files[0])
        for output_file in output_files:
            # Brute force the way we find the file from the output file
            for scope in SCOPE_ORDER:
                opts = [opt for opt in options if opt.scope == scope]
                class_name = scope.value
                if self.scope_doc_options[scope].is_settable_option:
                    class_name += "s"
                filename = (
                    "FDBException" if scope == Scope.ErrorPredicate else class_name
                )
                file_path = os.path.join(output_directory, filename + ".java")
                if file_path != output_file:
                    continue
                with open(file_path, "w", newline="\n") as out_file:
                    if self.scope_doc_options[scope].is_settable_option:
                        if scope == Scope.ErrorPredicate:
                            self._write_predicate_class(out_file, scope, opts)
                        else:
                            self._write_options_class(out_file, scope, opts)
                    else:
                        self._write_enum_class(out_file, scope, opts)


class PythonWriter(BindingWriter):
    type_map = {
        ParamType.NONE: "type(None)",
        ParamType.INT: "type(0)",
        ParamType.STRING: "type('')",
        ParamType.BYTES: "type(b'')",
    }

    @staticmethod
    def _python_line(option: Option) -> str:
        param_desc = "None" if option.param_desc is None else f'"{option.param_desc}"'
        return (
            f'    "{option.name}" : ({option.code}, "{option.comment}", '
            f"{PythonWriter.type_map[option.param_type]}, {param_desc}),"
        )

    @staticmethod
    def _write_dict(out_file, scope: Scope, options: Sequence[Option]) -> None:
        out_file.write(f"{scope.value} = {{\n")
        lines = [PythonWriter._python_line(opt) for opt in options if not opt.hidden]
        out_file.write("\n".join(lines))
        out_file.write("\n}\n\n")

    def write_files(self, output_file: List[str], options: Sequence[Option]) -> None:
        os.makedirs(os.path.dirname(output_file[0]) or ".", exist_ok=True)
        with open(output_file[0], "w", newline="\n") as out_file:
            out_file.write(
                textwrap.dedent(
                    """\
                    # FoundationDB Python API
                    # Copyright (c) 2013-2017 Apple Inc.

                    # Permission is hereby granted, free of charge, to any person obtaining a copy
                    # of this software and associated documentation files (the "Software"), to deal
                    # in the Software without restriction, including without limitation the rights
                    # to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
                    # copies of the Software, and to permit persons to whom the Software is
                    # furnished to do so, subject to the following conditions:

                    # The above copyright notice and this permission notice shall be included in
                    # all copies or substantial portions of the Software.

                    # THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
                    # IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
                    # FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
                    # AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
                    # LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
                    # OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
                    # THE SOFTWARE.

                    import types

                    """
                )
            )
            for scope in SCOPE_ORDER:
                scope_options = [opt for opt in options if opt.scope == scope]
                self._write_dict(out_file, scope, scope_options)


class RubyWriter(BindingWriter):
    type_map = {
        ParamType.NONE: "nil",
        ParamType.INT: "0",
        ParamType.STRING: "''",
        ParamType.BYTES: "''",
    }

    @staticmethod
    def _ruby_line(option: Option) -> str:
        param_desc = "nil" if option.param_desc is None else f'"{option.param_desc}"'
        return (
            f'    "{option.name.upper()}" => [{option.code}, "{option.comment}", '
            f"{RubyWriter.type_map[option.param_type]}, {param_desc}],"
        )

    @staticmethod
    def _write_hash(out_file, scope: Scope, options: Sequence[Option]) -> None:
        out_file.write(f"  @@{scope.value} = {{\n")
        lines = [RubyWriter._ruby_line(opt) for opt in options if not opt.hidden]
        out_file.write("\n".join(lines))
        out_file.write("\n  }\n\n")

    def write_files(self, output_file: List[str], options: Sequence[Option]) -> None:
        os.makedirs(os.path.dirname(output_file[0]) or ".", exist_ok=True)
        with open(output_file[0], "w", newline="\n") as out_file:
            out_file.write(
                textwrap.dedent(
                    """\
                    # FoundationDB Ruby API
                    # Copyright (c) 2013-2017 Apple Inc.

                    # Permission is hereby granted, free of charge, to any person obtaining a copy
                    # of this software and associated documentation files (the "Software"), to deal
                    # in the Software without restriction, including without limitation the rights
                    # to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
                    # copies of the Software, and to permit persons to whom the Software is
                    # furnished to do so, subject to the following conditions:

                    # The above copyright notice and this permission notice shall be included in
                    # all copies or substantial portions of the Software.

                    # THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
                    # IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
                    # FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
                    # AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
                    # LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
                    # OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
                    # THE SOFTWARE.

                    # Documentation for this API can be found at
                    # https://apple.github.io/foundationdb/api-ruby.html

                    module FDB
                    """
                )
            )
            for scope in SCOPE_ORDER:
                if scope == Scope.ErrorPredicate:
                    continue
                scope_options = [opt for opt in options if opt.scope == scope]
                self._write_hash(out_file, scope, scope_options)
            out_file.write("end\n")


def make_writer(binding: str) -> BindingWriter:
    writers = {
        "c": CWriter(),
        "cpp": CppWriter(),
        "java": JavaWriter(),
        "python": PythonWriter(),
        "ruby": RubyWriter(),
    }
    try:
        return writers[binding]
    except KeyError as exc:
        raise KeyError(f"Could not load language binding for `{binding}`") from exc


def main(argv: List[str]) -> int:
    if len(argv) < 3:
        usage()
        return 1

    input_file, binding = argv[0], argv[1]
    output_files = []
    output_files.extend(argv[2:])

    try:
        options = parse_options(input_file, binding)
    except Exception:
        return 1

    try:
        writer = make_writer(binding)
    except KeyError as exc:
        sys.stderr.write(f"{exc}\n")
        usage()
        return 31

    writer.write_files(output_files, options)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
