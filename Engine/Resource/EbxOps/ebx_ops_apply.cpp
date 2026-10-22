#include "ebx_ops_apply.h"

#include "check.h"
#include "ebx_writer_ext.h"
#include "ops.h"
#include "rawimage.h"

#include <exception>

namespace dingosdk::frostbite::ebx_ops {

Applied apply(std::span<const std::byte> bytes, const std::string& opsJson) {
    Applied out;
    try {
        auto doc = ::ebx::read_document(bytes);
        auto state = tool::make_state(doc);
        auto result = tool::apply_ops(doc, state, opsJson);
        out.lines = std::move(result.lines);
        tool::reslice_boxed(doc);
        ::ebx::WriteOptions options;
        options.listedBoxedRecords = state.listed;
        // a new array whose field path has no RFL2 group in the document is refused (as in ebxtool apply)
        options.refuseNewArrayHashes = true;
        ::ebx::WriteReport report;
        auto written = ::ebx::write_document_ext(doc, options, &report);
        if (!report.unknownTargets.empty()) {
            out.error = std::to_string(report.unknownTargets.size()) + " carried pointer(s) with unknown targets";
            return out;
        }
        const auto check = tool::check_ebx(written);
        if (!check.ok()) {
            out.error = "the edited copy does not check: " + (check.errors.empty() ? check.summary : check.errors.front());
            return out;
        }
        const auto again = ::ebx::read_document(written);
        ::ebx::WriteOptions twice;
        twice.listedBoxedRecords = tool::listed_records_from(again);
        if (::ebx::write_document_ext(again, twice, nullptr) != written) {
            out.error = "the edited copy does not write back the same";
            return out;
        }
        out.bytes = std::move(written);
    } catch (const std::exception& failed) {
        out.error = failed.what();
        out.bytes.clear();
    }
    return out;
}

} // namespace dingosdk::frostbite::ebx_ops
