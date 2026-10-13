// ToastQueue and MessageLog.
#include <string>

#include <lam/test/support.h>
#include <lam/ui/message_log.h>
#include <lam/ui/toasts.h>

using namespace lam;

TEST_CASE("toasts merge duplicates, fade and expire") {
    ToastQueue q(3);
    q.push("+1 Wheat");
    q.push("+1 Wheat");
    REQUIRE(q.toasts().size() == 1);
    CHECK(q.toasts().front().count == 2);
    q.push("a");
    q.push("b");
    q.push("c");
    CHECK(q.toasts().size() == 3);                              // oldest dropped
    CHECK(q.toasts().front().text.view() == "a");
    q.update(0.05f);
    CHECK(q.toasts().back().alpha < 1.0f);                      // fading in
    CHECK(q.toasts().back().alpha > 0.0f);
    q.update(10.0f);
    CHECK(q.toasts().empty());
}

TEST_CASE("toasts: timing, minimum life, fade out and restart on merge") {
    ToastQueue q;
    CHECK(q.capacity() == 8);
    q.push("saved", 1.0f, Color{1, 2, 3, 255});
    q.update(0.2f);
    REQUIRE(q.toasts().size() == 1);
    CHECK(q.toasts()[0].alpha == 1.0f);
    CHECK(q.toasts()[0].remaining == doctest::Approx(0.8f));
    q.update(0.6f);                                             // 0.2 s left: fading out
    CHECK(q.toasts()[0].alpha == doctest::Approx(0.5f));
    q.push("saved", 1.0f, Color{9, 9, 9, 255});                 // restarts, keeps it visible
    CHECK(q.toasts()[0].remaining == doctest::Approx(1.0f));
    CHECK(q.toasts()[0].count == 2);
    CHECK(q.toasts()[0].color == Color{9, 9, 9, 255});
    CHECK(q.toasts()[0].alpha == 1.0f);
    q.update(0.99f);
    CHECK(q.toasts().size() == 1);
    q.update(0.02f);
    CHECK(q.toasts().empty());
    q.push("short", 0.01f);                                     // at least 0.6 s
    q.update(0.5f);
    CHECK(q.toasts().size() == 1);
    q.push("nan", std::numeric_limits<float>::quiet_NaN());
    CHECK(q.toasts().back().remaining == doctest::Approx(2.5f));
    q.update(-1.0f);                                            // ignored
    q.update(std::numeric_limits<float>::infinity());
    CHECK(q.toasts().size() == 2);
    q.clear();
    CHECK(q.toasts().empty());
}

TEST_CASE("toasts: text truncates to the fixed buffer; capacity 0 becomes 1; no allocation") {
    ToastQueue one(0);
    CHECK(one.capacity() == 1);
    const std::string long_text(100, 'x');
    ToastQueue q(4);
    CHECK_NO_ALLOC(q.push(long_text));
    CHECK(q.toasts()[0].text.size() == 63);
    CHECK_NO_ALLOC({
        for (int i = 0; i < 100; ++i) {
            q.push(i % 2 ? "even" : "odd");
            q.push("unique", 0.7f);
            q.update(0.016f);
        }
    });
    CHECK(q.toasts().size() <= 4);
    std::uint16_t big = 0;
    for (int i = 0; i < 70000; ++i) q.push("spam");
    for (const auto &t : q.toasts())
        if (t.text.view() == "spam") big = t.count;
    CHECK(big == 0xFFFF);                                       // saturates
}

TEST_CASE("message log keeps the newest lines in order with their colours") {
    MessageLog log(3);
    CHECK(log.capacity() == 3);
    CHECK(log.size() == 0);
    log.add("one", Color{1, 0, 0, 255});
    log.add("two");
    CHECK(log.size() == 2);
    CHECK(log.line(0) == "one");
    CHECK(log.color(0) == Color{1, 0, 0, 255});
    CHECK(log.color(1) == kWhite);
    log.add("three");
    log.add("four", Color{4, 4, 4, 255});
    CHECK(log.size() == 3);
    CHECK(log.line(0) == "two");                                // "one" dropped
    CHECK(log.line(2) == "four");
    CHECK(log.color(2) == Color{4, 4, 4, 255});
    CHECK(log.added() == 4);
    log.clear();
    CHECK(log.size() == 0);
    CHECK(log.added() == 4);
    log.add("again");
    CHECK(log.line(0) == "again");
    MessageLog tiny(0);
    CHECK(tiny.capacity() == 1);
}

TEST_CASE("message log truncates to 96 bytes and never allocates after construction") {
    MessageLog log(8);
    const std::string long_text(300, 'y');
    CHECK_NO_ALLOC({
        for (int i = 0; i < 1000; ++i) log.add(long_text, Color{1, 2, 3, 4});
    });
    CHECK(log.size() == 8);
    CHECK(log.line(7).size() == 96);
}
