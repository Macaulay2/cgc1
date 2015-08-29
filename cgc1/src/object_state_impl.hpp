namespace cgc1
{
  namespace details
  {
    ALWAYS_INLINE inline bool is_aligned_properly(const object_state_t *os) noexcept
    {
      return os == align_pow2(os, c_align_pow2);
    }
    ALWAYS_INLINE inline object_state_t *object_state_t::from_object_start(void *v) noexcept
    {
      return reinterpret_cast<object_state_t *>(reinterpret_cast<uint8_t *>(v) - align(sizeof(object_state_t)));
    }
    ALWAYS_INLINE inline void
    object_state_t::set_all(object_state_t *next, bool in_use, bool next_valid, bool quasi_freed) noexcept
    {
      m_next = reinterpret_cast<size_t>(next) | static_cast<size_t>(in_use) | (static_cast<size_t>(next_valid) << 1) |
               (static_cast<size_t>(quasi_freed) << 2);
    }
    ALWAYS_INLINE inline void object_state_t::set_in_use(bool v) noexcept
    {
      size_t ptr = static_cast<size_t>(m_next);
      size_t iv = static_cast<size_t>(v);
      m_next = (ptr & size_inverse(1)) | (iv & 1);
    }
    ALWAYS_INLINE inline bool object_state_t::not_available() const noexcept
    {
      return 0 < (m_next & 5);
    }
    ALWAYS_INLINE inline bool object_state_t::in_use() const noexcept
    {
      return m_next & 1;
    }
    ALWAYS_INLINE inline bool object_state_t::quasi_freed() const noexcept
    {
      return (m_next & 4) > 0;
    }
    ALWAYS_INLINE inline void object_state_t::set_quasi_freed() noexcept
    {
      set_all(next(), false, next_valid(), true);
    }
    ALWAYS_INLINE inline void object_state_t::set_quasi_freed(bool val) noexcept
    {
      assert(!(val && in_use()));
      set_all(next(), in_use(), next_valid(), val);
    }
    ALWAYS_INLINE inline void object_state_t::set_next_valid(bool v) noexcept
    {
      size_t ptr = static_cast<size_t>(m_next);
      size_t iv = static_cast<size_t>(v) << 1;
      m_next = (ptr & size_inverse(2)) | (iv & 2);
    }
    ALWAYS_INLINE inline bool object_state_t::next_valid() const noexcept
    {
      return (static_cast<size_t>(m_next) & 2) > 0;
    }
    ALWAYS_INLINE inline object_state_t *object_state_t::next() const noexcept
    {
      return reinterpret_cast<object_state_t *>(m_next & size_inverse(7));
    }
    ALWAYS_INLINE inline void object_state_t::set_next(object_state_t *state) noexcept
    {
      size_t ptr = reinterpret_cast<size_t>(state);
      //      size_t iv = static_cast<size_t>(not_available());
      m_next = (ptr & static_cast<size_t>(-4)) | (m_next & 1); //(iv & 1);
    }
    ALWAYS_INLINE inline uint8_t *object_state_t::object_start() const noexcept
    {
      return const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(this) + align(sizeof(object_state_t)));
    }
    ALWAYS_INLINE inline size_t object_state_t::object_size() const noexcept
    {
      // It is invariant that object_start() > next for all valid objects.
      return static_cast<size_t>(reinterpret_cast<uint8_t *>(next()) - object_start());
    }
    ALWAYS_INLINE inline uint8_t *object_state_t::object_end() const noexcept
    {
      return const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(this) + align(sizeof(object_state_t)) + object_size());
    }
    ALWAYS_INLINE inline user_data_base_t *object_state_t::user_data() const noexcept
    {
      return reinterpret_cast<user_data_base_t *>(m_user_data & (~static_cast<size_t>(7)));
    }
    ALWAYS_INLINE inline void object_state_t::set_user_data(void *user_data) noexcept
    {
      assert(user_data == align_pow2(user_data, 3));
      m_user_data = reinterpret_cast<size_t>(user_data) | user_flags();
    }
    ALWAYS_INLINE inline size_t object_state_t::user_flags() const noexcept
    {
      return m_user_data & 7;
    }
    ALWAYS_INLINE inline void object_state_t::set_user_flags(size_t flags) noexcept
    {
      assert(flags < 8);
      m_user_data = reinterpret_cast<size_t>(user_data()) | flags;
    }
  }
}
